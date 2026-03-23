#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "argparse/argparse.hpp"
#include "buffer/lru_k_replacer.h"
#include "common/config.h"
#include "fmt/core.h"

namespace {

using bustub::frame_id_t;
using bustub::page_id_t;

struct BenchConfig {
  std::string workload_{"mixed"};
  std::string trace_file_{};
  size_t frames_{128};
  size_t pages_{10000};
  size_t ops_{1000000};
  size_t min_k_{1};
  size_t max_k_{8};
  size_t step_{1};
  size_t repeat_{3};
  size_t hotset_{256};
  size_t scan_burst_{256};
  size_t hot_burst_{2048};
  size_t warmup_ops_{0};
  size_t trace_limit_{0};
  double hot_prob_{0.9};
  uint64_t seed_{42};
};

struct RunStats {
  uint64_t hits_{0};
  uint64_t misses_{0};
  uint64_t evictions_{0};
  double ns_per_op_{0.0};

  auto HitRate() const -> double {
    const auto total = hits_ + misses_;
    if (total == 0) {
      return 0.0;
    }
    return static_cast<double>(hits_) / static_cast<double>(total);
  }
};

auto ParseSize(const std::string &value, const std::string &name) -> size_t {
  try {
    size_t parsed_len = 0;
    auto parsed = std::stoull(value, &parsed_len);
    if (parsed_len != value.size()) {
      throw std::invalid_argument("extra characters");
    }
    return static_cast<size_t>(parsed);
  } catch (const std::exception &) {
    throw std::runtime_error(fmt::format("invalid value for {}: {}", name, value));
  }
}

auto ParsePositiveSize(const std::string &value, const std::string &name) -> size_t {
  auto parsed = ParseSize(value, name);
  if (parsed == 0) {
    throw std::runtime_error(fmt::format("{} must be > 0", name));
  }
  return parsed;
}

auto ParseProbability(const std::string &value, const std::string &name) -> double {
  try {
    size_t parsed_len = 0;
    auto parsed = std::stod(value, &parsed_len);
    if (parsed_len != value.size()) {
      throw std::invalid_argument("extra characters");
    }
    if (parsed < 0.0 || parsed > 1.0) {
      throw std::out_of_range("not in [0, 1]");
    }
    return parsed;
  } catch (const std::exception &) {
    throw std::runtime_error(fmt::format("{} must be a number in [0, 1], got {}", name, value));
  }
}

void ValidatePageCount(size_t page_count, const std::string &name) {
  if (page_count > static_cast<size_t>(std::numeric_limits<page_id_t>::max())) {
    throw std::runtime_error(
        fmt::format("{} must be <= {}, got {}", name, std::numeric_limits<page_id_t>::max(), page_count));
  }
}

auto ParseConfig(const argparse::ArgumentParser &program) -> BenchConfig {
  BenchConfig config;
  config.workload_ = program.get<std::string>("--workload");
  config.trace_file_ = program.get<std::string>("--trace-file");
  config.frames_ = ParsePositiveSize(program.get<std::string>("--frames"), "--frames");
  config.pages_ = ParsePositiveSize(program.get<std::string>("--pages"), "--pages");
  config.ops_ = ParsePositiveSize(program.get<std::string>("--ops"), "--ops");
  config.min_k_ = ParsePositiveSize(program.get<std::string>("--min-k"), "--min-k");
  config.max_k_ = ParsePositiveSize(program.get<std::string>("--max-k"), "--max-k");
  config.step_ = ParsePositiveSize(program.get<std::string>("--step"), "--step");
  config.repeat_ = ParsePositiveSize(program.get<std::string>("--repeat"), "--repeat");
  config.hotset_ = ParseSize(program.get<std::string>("--hotset"), "--hotset");
  config.scan_burst_ = ParsePositiveSize(program.get<std::string>("--scan-burst"), "--scan-burst");
  config.hot_burst_ = ParsePositiveSize(program.get<std::string>("--hot-burst"), "--hot-burst");
  config.warmup_ops_ = ParseSize(program.get<std::string>("--warmup-ops"), "--warmup-ops");
  config.trace_limit_ = ParseSize(program.get<std::string>("--trace-limit"), "--trace-limit");
  config.hot_prob_ = ParseProbability(program.get<std::string>("--hot-prob"), "--hot-prob");
  config.seed_ = static_cast<uint64_t>(ParseSize(program.get<std::string>("--seed"), "--seed"));

  ValidatePageCount(config.pages_, "--pages");

  if (config.min_k_ > config.max_k_) {
    throw std::runtime_error("--min-k must be <= --max-k");
  }

  if (config.workload_ != "hotspot" && config.workload_ != "scan" && config.workload_ != "mixed" &&
      config.workload_ != "trace") {
    throw std::runtime_error("--workload must be one of: hotspot, scan, mixed, trace");
  }

  if (config.workload_ == "trace" && config.trace_file_.empty()) {
    throw std::runtime_error("--trace-file is required when --workload=trace");
  }

  return config;
}

auto SampleHotspotPage(std::mt19937_64 *generator, size_t pages, size_t hotset, double hot_prob) -> page_id_t {
  if (pages == 0) {
    throw std::runtime_error("pages must be > 0");
  }

  hotset = std::min(hotset, pages);
  if (hotset == 0 || hotset == pages) {
    std::uniform_int_distribution<int64_t> full_dist(0, static_cast<int64_t>(pages - 1));
    return static_cast<page_id_t>(full_dist(*generator));
  }

  std::bernoulli_distribution choose_hot(hot_prob);
  if (choose_hot(*generator)) {
    std::uniform_int_distribution<int64_t> hot_dist(0, static_cast<int64_t>(hotset - 1));
    return static_cast<page_id_t>(hot_dist(*generator));
  }

  std::uniform_int_distribution<int64_t> cold_dist(static_cast<int64_t>(hotset), static_cast<int64_t>(pages - 1));
  return static_cast<page_id_t>(cold_dist(*generator));
}

auto GenerateHotspotTrace(const BenchConfig &config) -> std::vector<page_id_t> {
  std::mt19937_64 generator(config.seed_);
  std::vector<page_id_t> trace;
  trace.reserve(config.ops_);
  for (size_t i = 0; i < config.ops_; i++) {
    trace.push_back(SampleHotspotPage(&generator, config.pages_, config.hotset_, config.hot_prob_));
  }
  return trace;
}

auto GenerateScanTrace(const BenchConfig &config) -> std::vector<page_id_t> {
  std::vector<page_id_t> trace;
  trace.reserve(config.ops_);
  for (size_t i = 0; i < config.ops_; i++) {
    trace.push_back(static_cast<page_id_t>(i % config.pages_));
  }
  return trace;
}

auto GenerateMixedTrace(const BenchConfig &config) -> std::vector<page_id_t> {
  std::mt19937_64 generator(config.seed_);
  std::vector<page_id_t> trace;
  trace.reserve(config.ops_);
  size_t scan_cursor = 0;
  while (trace.size() < config.ops_) {
    for (size_t i = 0; i < config.scan_burst_ && trace.size() < config.ops_; i++) {
      trace.push_back(static_cast<page_id_t>(scan_cursor % config.pages_));
      scan_cursor++;
    }
    for (size_t i = 0; i < config.hot_burst_ && trace.size() < config.ops_; i++) {
      trace.push_back(SampleHotspotPage(&generator, config.pages_, config.hotset_, config.hot_prob_));
    }
  }

  return trace;
}

auto LoadTraceFromFile(const BenchConfig &config) -> std::vector<page_id_t> {
  std::ifstream trace_file(config.trace_file_);
  if (!trace_file) {
    throw std::runtime_error(fmt::format("failed to open trace file: {}", config.trace_file_));
  }

  std::vector<page_id_t> trace;
  if (config.trace_limit_ > 0) {
    trace.reserve(config.trace_limit_);
  }

  int64_t raw_page_id = 0;
  while (trace_file >> raw_page_id) {
    if (raw_page_id < 0 || raw_page_id > std::numeric_limits<page_id_t>::max()) {
      throw std::runtime_error(
          fmt::format("trace page id must be in [0, {}], got {}", std::numeric_limits<page_id_t>::max(), raw_page_id));
    }
    trace.push_back(static_cast<page_id_t>(raw_page_id));
    if (config.trace_limit_ > 0 && trace.size() >= config.trace_limit_) {
      break;
    }
  }

  if (trace.empty()) {
    throw std::runtime_error("trace file did not contain any page ids");
  }

  return trace;
}

auto BuildTrace(const BenchConfig &config) -> std::vector<page_id_t> {
  if (config.workload_ == "trace") {
    return LoadTraceFromFile(config);
  }
  if (config.workload_ == "hotspot") {
    return GenerateHotspotTrace(config);
  }
  if (config.workload_ == "scan") {
    return GenerateScanTrace(config);
  }
  return GenerateMixedTrace(config);
}

class ReplacerSimulator {
 public:
  ReplacerSimulator(size_t frames, size_t k) : replacer_(frames, k), frame_to_page_(frames, bustub::INVALID_PAGE_ID) {
    free_frames_.reserve(frames);
    for (size_t frame = 0; frame < frames; frame++) {
      free_frames_.push_back(static_cast<frame_id_t>(frame));
    }
  }

  auto Run(const std::vector<page_id_t> &trace, size_t warmup_ops) -> RunStats {
    if (warmup_ops >= trace.size()) {
      throw std::runtime_error("--warmup-ops must be smaller than the trace length");
    }

    RunStats stats;
    std::chrono::steady_clock::time_point start_time;
    bool timer_started = false;

    for (size_t i = 0; i < trace.size(); i++) {
      if (!timer_started && i == warmup_ops) {
        start_time = std::chrono::steady_clock::now();
        timer_started = true;
      }
      const bool measure = i >= warmup_ops;
      Access(trace[i], measure, &stats);
    }

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const auto measured_ops = trace.size() - warmup_ops;
    stats.ns_per_op_ =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / measured_ops;
    return stats;
  }

 private:
  void Touch(frame_id_t frame_id) {
    replacer_.RecordAccess(frame_id);
    replacer_.SetEvictable(frame_id, false);
    replacer_.SetEvictable(frame_id, true);
  }

  void Access(page_id_t page_id, bool measure, RunStats *stats) {
    auto page_iter = page_to_frame_.find(page_id);
    if (page_iter != page_to_frame_.end()) {
      if (measure) {
        stats->hits_++;
      }
      Touch(page_iter->second);
      return;
    }

    if (measure) {
      stats->misses_++;
    }

    frame_id_t frame_id = bustub::INVALID_PAGE_ID;
    if (!free_frames_.empty()) {
      frame_id = free_frames_.back();
      free_frames_.pop_back();
    } else {
      bool evicted = replacer_.Evict(&frame_id);
      if (!evicted) {
        throw std::runtime_error("replacer reported no evictable frame in the simulator");
      }
      page_to_frame_.erase(frame_to_page_[frame_id]);
      if (measure) {
        stats->evictions_++;
      }
    }

    frame_to_page_[frame_id] = page_id;
    page_to_frame_[page_id] = frame_id;
    Touch(frame_id);
  }

  bustub::LRUKReplacer replacer_;
  std::vector<page_id_t> frame_to_page_;
  std::unordered_map<page_id_t, frame_id_t> page_to_frame_;
  std::vector<frame_id_t> free_frames_;
};

struct BenchResult {
  size_t k_{0};
  uint64_t hits_{0};
  uint64_t misses_{0};
  uint64_t evictions_{0};
  double hit_rate_{0.0};
  double ns_per_op_{0.0};
};

auto RunForK(const BenchConfig &config, const std::vector<page_id_t> &trace, size_t k) -> BenchResult {
  RunStats first_run;
  double ns_sum = 0.0;

  for (size_t repeat = 0; repeat < config.repeat_; repeat++) {
    ReplacerSimulator simulator(config.frames_, k);
    auto run = simulator.Run(trace, config.warmup_ops_);
    if (repeat == 0) {
      first_run = run;
    }
    ns_sum += run.ns_per_op_;
  }

  BenchResult result;
  result.k_ = k;
  result.hits_ = first_run.hits_;
  result.misses_ = first_run.misses_;
  result.evictions_ = first_run.evictions_;
  result.hit_rate_ = first_run.HitRate();
  result.ns_per_op_ = ns_sum / static_cast<double>(config.repeat_);
  return result;
}

void PrintHeader(const BenchConfig &config, const std::vector<page_id_t> &trace) {
  const auto measured_ops = trace.size() - config.warmup_ops_;
  fmt::print("workload={} frames={} trace_ops={} measured_ops={} repeats={} k_range=[{},{}] step={}\n", config.workload_,
             config.frames_, trace.size(), measured_ops, config.repeat_, config.min_k_, config.max_k_, config.step_);

  if (config.workload_ == "trace") {
    fmt::print("trace_file={}\n", config.trace_file_);
  } else {
    fmt::print("pages={} hotset={} hot_prob={:.2f} scan_burst={} hot_burst={} seed={}\n", config.pages_,
               std::min(config.hotset_, config.pages_), config.hot_prob_, config.scan_burst_, config.hot_burst_,
               config.seed_);
  }

  if (config.pages_ <= config.frames_ && config.workload_ != "trace") {
    fmt::print("warning: pages <= frames, synthetic hit rates will quickly saturate near 100%\n");
  }

  fmt::print("{:>4} {:>12} {:>12} {:>12} {:>12} {:>12}\n", "k", "hits", "misses", "hit_rate", "evictions", "ns/op");
}

void PrintResult(const BenchResult &result) {
  fmt::print("{:>4} {:>12} {:>12} {:>11.4f}% {:>12} {:>12.2f}\n", result.k_, result.hits_, result.misses_,
             result.hit_rate_ * 100.0, result.evictions_, result.ns_per_op_);
}

auto BestResult(const std::vector<BenchResult> &results) -> BenchResult {
  return *std::max_element(results.begin(), results.end(), [](const BenchResult &lhs, const BenchResult &rhs) {
    if (lhs.hit_rate_ != rhs.hit_rate_) {
      return lhs.hit_rate_ < rhs.hit_rate_;
    }
    return lhs.ns_per_op_ > rhs.ns_per_op_;
  });
}

}  // namespace

auto main(int argc, char **argv) -> int {  // NOLINT
  argparse::ArgumentParser program("bustub-lru-k-bench");
  program.add_argument("--workload")
      .help("workload type: hotspot, scan, mixed, trace")
      .default_value(std::string("mixed"));
  program.add_argument("--trace-file")
      .help("page trace file for --workload=trace, one page id per token")
      .default_value(std::string(""));
  program.add_argument("--frames").help("buffer pool frame count").default_value(std::string("128"));
  program.add_argument("--pages").help("logical page count for synthetic traces").default_value(std::string("10000"));
  program.add_argument("--ops").help("access count for synthetic traces").default_value(std::string("1000000"));
  program.add_argument("--min-k").help("smallest k to test").default_value(std::string("1"));
  program.add_argument("--max-k").help("largest k to test").default_value(std::string("8"));
  program.add_argument("--step").help("increment when sweeping k").default_value(std::string("1"));
  program.add_argument("--repeat").help("repeat runs per k and average ns/op").default_value(std::string("3"));
  program.add_argument("--hotset").help("size of the hot set for hotspot/mixed traces").default_value(std::string("256"));
  program.add_argument("--hot-prob")
      .help("probability of touching the hot set when sampling hotspot accesses")
      .default_value(std::string("0.9"));
  program.add_argument("--scan-burst")
      .help("sequential scan burst length for mixed traces")
      .default_value(std::string("256"));
  program.add_argument("--hot-burst")
      .help("hotspot burst length for mixed traces")
      .default_value(std::string("2048"));
  program.add_argument("--seed").help("random seed for synthetic traces").default_value(std::string("42"));
  program.add_argument("--warmup-ops")
      .help("simulate these first accesses without counting them in the final stats")
      .default_value(std::string("0"));
  program.add_argument("--trace-limit")
      .help("optional maximum number of accesses to read from a trace file, 0 means no limit")
      .default_value(std::string("0"));

  try {
    program.parse_args(argc, argv);
  } catch (const std::runtime_error &err) {
    std::cerr << err.what() << std::endl;
    std::cerr << program;
    return 1;
  }

  try {
    const auto config = ParseConfig(program);
    const auto trace = BuildTrace(config);

    if (config.warmup_ops_ >= trace.size()) {
      throw std::runtime_error("--warmup-ops must be smaller than the trace length");
    }

    std::vector<BenchResult> results;
    for (size_t k = config.min_k_; k <= config.max_k_; k += config.step_) {
      results.push_back(RunForK(config, trace, k));
    }

    PrintHeader(config, trace);
    for (const auto &result : results) {
      PrintResult(result);
    }

    const auto best = BestResult(results);
    fmt::print("\nbest_k_by_hit_rate={} hit_rate={:.4f}% ns/op={:.2f}\n", best.k_, best.hit_rate_ * 100.0,
               best.ns_per_op_);
  } catch (const std::exception &err) {
    std::cerr << err.what() << std::endl;
    return 1;
  }

  return 0;
}
