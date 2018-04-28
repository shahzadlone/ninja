// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "build.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <functional>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#if defined(__SVR4) && defined(__sun)
#include <sys/termios.h>
#endif

#include "build_log.h"
#include "clparser.h"
#include "debug_flags.h"
#include "depfile_parser.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "graph.h"
#include "state.h"
#include "subprocess.h"
#include "util.h"

namespace {

/// A CommandRunner that doesn't actually run the commands.
struct DryRunCommandRunner final : public CommandRunner {
  virtual ~DryRunCommandRunner() {}

  // Overridden from CommandRunner:
  bool CanRunMore() override final;
  bool StartCommand(Edge* edge) override final;
  bool WaitForCommand(Result* result) override final;

 private:
  std::queue<Edge*> finished_;
};

bool DryRunCommandRunner::CanRunMore() {
  return true;
}

bool DryRunCommandRunner::StartCommand(Edge* edge) {
  finished_.push(edge);
  return true;
}

bool DryRunCommandRunner::WaitForCommand(Result* result) {
   if (finished_.empty())
     return false;

   result->status = ExitSuccess;
   result->edge = finished_.front();
   finished_.pop();
   return true;
}

}  // namespace

BuildStatus::BuildStatus(const BuildConfig& config)
    : config_(config),
      start_time_millis_(GetTimeMillis()),
      started_edges_(0), finished_edges_(0), total_edges_(0),
      progress_status_format_(nullptr),
      overall_rate_(), current_rate_(config.parallelism) {

  // Don't do anything fancy in verbose mode.
  if (config_.verbosity != BuildConfig::NORMAL)
    printer_.set_smart_terminal(false);

  progress_status_format_ = getenv("NINJA_STATUS");
  if (!progress_status_format_)
    progress_status_format_ = "[%f/%t] ";
}

void BuildStatus::PlanHasTotalEdges(int total) {
  total_edges_ = total;
}

void BuildStatus::BuildEdgeStarted(Edge* edge) {
  int start_time = (int)(GetTimeMillis() - start_time_millis_);
  running_edges_.insert(std::make_pair(edge, start_time));
  ++started_edges_;

  if (edge->use_console() || printer_.is_smart_terminal())
    PrintStatus(edge, kEdgeStarted);

  if (edge->use_console())
    printer_.SetConsoleLocked(true);
}

void BuildStatus::BuildEdgeFinished(Edge* edge,
                                    bool success,
                                    const std::string& output,
                                    int* start_time,
                                    int* end_time) {
  int64_t now = GetTimeMillis();

  ++finished_edges_;

  RunningEdgeMap::iterator i = running_edges_.find(edge);
  *start_time = i->second;
  *end_time = (int)(now - start_time_millis_);
  running_edges_.erase(i);

  if (edge->use_console())
    printer_.SetConsoleLocked(false);

  if (config_.verbosity == BuildConfig::QUIET)
    return;

  if (!edge->use_console())
    PrintStatus(edge, kEdgeFinished);
  if (printer_.is_smart_terminal()) {
    int oldest_start = INT_MAX;
    Edge* oldest = nullptr;
    for (i = running_edges_.begin(); i != running_edges_.end(); i++) {
      if (i->second < oldest_start) {
        oldest_start = i->second;
        oldest = i->first;
      }
    }
    if (oldest)
      PrintStatus(oldest, kEdgeRunning);
  }

  // Print the command that is spewing before printing its output.
  if (!success) {
    std::string outputs;
    for (const auto & item : edge->outputs_)
      outputs += item->path() + " ";

    printer_.PrintOnNewLine("FAILED: " + outputs + "\n");
    printer_.PrintOnNewLine(edge->EvaluateCommand() + "\n");
  }

  if (!output.empty()) {
    // ninja sets stdout and stderr of subprocesses to a pipe, to be able to
    // check if the output is empty. Some compilers, e.g. clang, check
    // isatty(stderr) to decide if they should print colored output.
    // To make it possible to use colored output with ninja, subprocesses should
    // be run with a flag that forces them to always print color escape codes.
    // To make sure these escape codes don't show up in a file if ninja's output
    // is piped to a file, ninja strips ansi escape codes again if it's not
    // writing to a |smart_terminal_|.
    // (Launching subprocesses in pseudo ttys doesn't work because there are
    // only a few hundred available on some systems, and ninja can launch
    // thousands of parallel compile commands.)
    // TODO: There should be a flag to disable escape code stripping.
    std::string final_output;
    if (!printer_.is_smart_terminal())
      final_output = StripAnsiEscapeCodes(output);
    else
      final_output = output;

#ifdef _WIN32
    // Fix extra CR being added on Windows, writing out CR CR LF (#773)
    _setmode(_fileno(stdout), _O_BINARY);  // Begin Windows extra CR fix
#endif

    printer_.PrintOnNewLine(final_output);

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_TEXT);  // End Windows extra CR fix
#endif
  }
}

void BuildStatus::BuildStarted() {
  overall_rate_.Restart();
  current_rate_.Restart();
}

void BuildStatus::BuildFinished() {
  printer_.SetConsoleLocked(false);
  printer_.PrintOnNewLine("");
}

std::string BuildStatus::FormatProgressStatus(
    const char* progress_status_format, EdgeStatus status) const {
  std::string out;
  char buf[32];
  int percent;
  for (const char* s = progress_status_format; *s != '\0'; ++s) {
    if (*s == '%') {
      ++s;
      switch (*s) {
      case '%':
        out.push_back('%');
        break;

        // Started edges.
      case 's':
        snprintf(buf, sizeof(buf), "%d", started_edges_);
        out += buf;
        break;

        // Total edges.
      case 't':
        snprintf(buf, sizeof(buf), "%d", total_edges_);
        out += buf;
        break;

        // Running edges.
      case 'r': {
        int running_edges = started_edges_ - finished_edges_;
        // count the edge that just finished as a running edge
        if (status == kEdgeFinished)
          running_edges++;
        snprintf(buf, sizeof(buf), "%d", running_edges);
        out += buf;
        break;
      }

        // Unstarted edges.
      case 'u':
        snprintf(buf, sizeof(buf), "%d", total_edges_ - started_edges_);
        out += buf;
        break;

        // Finished edges.
      case 'f':
        snprintf(buf, sizeof(buf), "%d", finished_edges_);
        out += buf;
        break;

        // Overall finished edges per second.
      case 'o':
        overall_rate_.UpdateRate(finished_edges_);
        SnprintfRate(overall_rate_.rate(), buf, "%.1f");
        out += buf;
        break;

        // Current rate, average over the last '-j' jobs.
      case 'c':
        current_rate_.UpdateRate(finished_edges_);
        SnprintfRate(current_rate_.rate(), buf, "%.1f");
        out += buf;
        break;

        // Percentage
      case 'p':
        percent = (100 * finished_edges_) / total_edges_;
        snprintf(buf, sizeof(buf), "%3i%%", percent);
        out += buf;
        break;

      case 'e': {
        double elapsed = overall_rate_.Elapsed();
        snprintf(buf, sizeof(buf), "%.3f", elapsed);
        out += buf;
        break;
      }

      default:
        Fatal("unknown placeholder '%%%c' in $NINJA_STATUS", *s);
        return "";
      }
    } else {
      out.push_back(*s);
    }
  }

  return out;
}

void BuildStatus::PrintStatus(Edge* edge, EdgeStatus status) {
  if (config_.verbosity == BuildConfig::QUIET)
    return;

  bool force_full_command = config_.verbosity == BuildConfig::VERBOSE;

  std::string to_print = edge->GetBinding("description");
  if (to_print.empty() || force_full_command)
    to_print = edge->GetBinding("command");

  to_print = FormatProgressStatus(progress_status_format_, status) + to_print;

  printer_.Print(to_print,
                 force_full_command ? LinePrinter::FULL : LinePrinter::ELIDE);
}

Plan::Plan() : command_edges_(0), wanted_edges_(0) {}

void Plan::Reset() {
  command_edges_ = 0;
  wanted_edges_ = 0;
  ready_.clear();
  want_.clear();
}

bool Plan::AddTarget(Node* node, std::string* err) {
  return AddSubTarget(node, nullptr, err);
}

bool Plan::AddSubTarget(Node* node, Node* dependent, std::string* err) {
  Edge* edge = node->in_edge();
  if (!edge) {  // Leaf node.
    if (node->dirty()) {
      std::string referenced;
      if (dependent)
        referenced = ", needed by '" + dependent->path() + "',";
      *err = "'" + node->path() + "'" + referenced + " missing "
             "and no known rule to make it";
    }
    return false;
  }

  if (edge->outputs_ready())
    return false;  // Don't need to do anything.

  // If an entry in want_ does not already exist for edge, create an entry which
  // maps to kWantNothing, indicating that we do not want to build this entry itself.
  std::pair<std::map<Edge*, Want>::iterator, bool> want_ins =
    want_.insert(std::make_pair(edge, kWantNothing));
  Want& want = want_ins.first->second;

  // If we do need to build edge and we haven't already marked it as wanted,
  // mark it now.
  if (node->dirty() && want == kWantNothing) {
    want = kWantToStart;
    ++wanted_edges_;
    if (edge->AllInputsReady())
      ScheduleWork(want_ins.first);
    if (!edge->is_phony())
      ++command_edges_;
  }

  if (!want_ins.second)
    return true;  // We've already processed the inputs.

  for (const auto & item : edge->inputs_)
  {
    if (!AddSubTarget(item, node, err) && !err->empty())
    {
        return false;
    }
  }

  return true;
}

Edge* Plan::FindWork() {
  if (ready_.empty())
    return nullptr;
  std::set<Edge*>::iterator e = ready_.begin();
  Edge* edge = *e;
  ready_.erase(e);
  return edge;
}

void Plan::ScheduleWork(std::map<Edge*, Want>::iterator want_e) {
  if (want_e->second == kWantToFinish) {
    // This edge has already been scheduled.  We can get here again if an edge
    // and one of its dependencies share an order-only input, or if a node
    // duplicates an out edge (see https://github.com/ninja-build/ninja/pull/519).
    // Avoid scheduling the work again.
    return;
  }
  assert(want_e->second == kWantToStart);
  want_e->second = kWantToFinish;

  Edge* edge = want_e->first;
  Pool* pool = edge->pool();
  if (pool->ShouldDelayEdge()) {
    pool->DelayEdge(edge);
    pool->RetrieveReadyEdges(&ready_);
  } else {
    pool->EdgeScheduled(*edge);
    ready_.insert(edge);
  }
}

void Plan::EdgeFinished(Edge* edge, EdgeResult result)
{
  std::map<Edge*, Want>::iterator e = want_.find(edge);
  assert(e != want_.end());
  bool directly_wanted = e->second != kWantNothing;

  // See if this job frees up any delayed jobs.
  if (directly_wanted)
    edge->pool()->EdgeFinished(*edge);
  edge->pool()->RetrieveReadyEdges(&ready_);

  // The rest of this function only applies to successful commands.
  if (result != kEdgeSucceeded)
    return;

  if (directly_wanted)
    --wanted_edges_;
  want_.erase(e);
  edge->outputs_ready_ = true;

  // Check off any nodes we were waiting for with this edge.
  for(const auto & item : edge->outputs_)
  {
    NodeFinished(item);
  }
}

void Plan::NodeFinished(Node* node) {
  // See if we we want any edges from this node.
  for (const auto & item : node->out_edges()) {
    std::map<Edge*, Want>::iterator want_e = want_.find(item);
    if (want_e == want_.end())
      continue;

    // See if the edge is now ready.
    if (item->AllInputsReady()) {
      if (want_e->second != kWantNothing) {
        ScheduleWork(want_e);
      } else {
        // We do not need to build this edge, but we might need to build one of
        // its dependents.
        EdgeFinished(item, kEdgeSucceeded);
      }
    }
  }
}

bool Plan::CleanNode(DependencyScan* scan, Node* node, std::string* err) {
  node->set_dirty(false);

  for (const auto & item : node->out_edges())
  {
    // Don't process edges that we don't actually want.
    std::map<Edge*, Want>::iterator want_e = want_.find(item);
    if (want_e == want_.end() || want_e->second == kWantNothing)
      continue;

    // Don't attempt to clean an edge if it failed to load deps.
    if (item->deps_missing_)
      continue;

    // If all non-order-only inputs for this edge are now clean,
    // we might have changed the dirty state of the outputs.
    std::vector<Node*>::iterator
        begin = item->inputs_.begin(),
        end = item->inputs_.end() - item->order_only_deps_;
#if __cplusplus < 201703L
#define MEM_FN std::mem_fun
#else
#define MEM_FN std::mem_fn  // mem_fun was removed in C++17.
#endif
    if (find_if(begin, end, MEM_FN(&Node::dirty)) == end) {
      // Recompute most_recent_input.
      Node* most_recent_input = nullptr;
      for (std::vector<Node*>::iterator i = begin; i != end; ++i) {
        if (!most_recent_input || (*i)->mtime() > most_recent_input->mtime())
          most_recent_input = *i;
      }

      // Now, this edge is dirty if any of the outputs are dirty.
      // If the edge isn't dirty, clean the outputs and mark the edge as not
      // wanted.
      bool outputs_dirty = false;
      if (!scan->RecomputeOutputsDirty(item, most_recent_input,
                                       &outputs_dirty, err)) {
        return false;
      }
      if (!outputs_dirty) {
        for (const auto & inner : item->outputs_)
        {
          if (!CleanNode(scan, inner, err))
            return false;
        }

        want_e->second = kWantNothing;
        --wanted_edges_;
        if (!item->is_phony())
          --command_edges_;
      }
    }
  }
  return true;
}

void Plan::Dump() {
  printf("pending: %d\n", (int)want_.size());
  for (const auto & item : want_) {
    if (item.second != kWantNothing)
      printf("want ");
    item.first->Dump();
  }
  printf("ready: %d\n", (int)ready_.size());
}

struct RealCommandRunner final : public CommandRunner {
  explicit RealCommandRunner(const BuildConfig& config) : config_(config) {}
  virtual ~RealCommandRunner() {}
  bool CanRunMore() override final;
  bool StartCommand(Edge* edge) override final;
  bool WaitForCommand(Result* result) override final;
  std::vector<Edge*> GetActiveEdges() override final;
  void Abort() override final;

  const BuildConfig& config_;
  SubprocessSet subprocs_;
  std::map<Subprocess*, Edge*> subproc_to_edge_;
};

std::vector<Edge*> RealCommandRunner::GetActiveEdges() {
  std::vector<Edge*> edges;
  for (const auto & item : subproc_to_edge_)
  {
    edges.push_back(item.second);
  }
  return edges;
}

void RealCommandRunner::Abort() {
  subprocs_.Clear();
}

bool RealCommandRunner::CanRunMore() {
  size_t subproc_number =
      subprocs_.running_.size() + subprocs_.finished_.size();
  return (int)subproc_number < config_.parallelism
    && ((subprocs_.running_.empty() || config_.max_load_average <= 0.0f)
        || GetLoadAverage() < config_.max_load_average);
}

bool RealCommandRunner::StartCommand(Edge* edge) {
  std::string command = edge->EvaluateCommand();
  Subprocess* subproc = subprocs_.Add(command, edge->use_console());
  if (!subproc)
    return false;
  subproc_to_edge_.insert(std::make_pair(subproc, edge));

  return true;
}

bool RealCommandRunner::WaitForCommand(Result* result) {
  Subprocess* subproc;
  while ((subproc = subprocs_.NextFinished()) == nullptr) {
    bool interrupted = subprocs_.DoWork();
    if (interrupted)
      return false;
  }

  result->status = subproc->Finish();
  result->output = subproc->GetOutput();

  std::map<Subprocess*, Edge*>::iterator e = subproc_to_edge_.find(subproc);
  result->edge = e->second;
  subproc_to_edge_.erase(e);

  delete subproc;
  return true;
}

Builder::Builder(State* state, const BuildConfig& config,
                 BuildLog* build_log, DepsLog* deps_log,
                 DiskInterface* disk_interface)
    : state_(state), config_(config), disk_interface_(disk_interface),
      scan_(state, build_log, deps_log, disk_interface) {
  status_ = new BuildStatus(config);
}

Builder::~Builder() {
  Cleanup();
}

void Builder::Cleanup() {
  if (command_runner_.get()) {
    std::vector<Edge*> active_edges = command_runner_->GetActiveEdges();
    command_runner_->Abort();

    for (const auto & item : active_edges)
    {
      std::string depfile = item->GetUnescapedDepfile();
      for (const auto & inner : item->outputs_)
      {
        // Only delete this output if it was actually modified.  This is
        // important for things like the generator where we don't want to
        // delete the manifest file if we can avoid it.  But if the rule
        // uses a depfile, always delete.  (Consider the case where we
        // need to rebuild an output because of a modified header file
        // mentioned in a depfile, and the command touches its depfile
        // but is interrupted before it touches its output file.)
        std::string err;
        TimeStamp new_mtime = disk_interface_->Stat(inner->path(), &err);
        if (new_mtime == -1)  // Log and ignore Stat() errors.
          Error("%s", err.c_str());
        if (!depfile.empty() || inner->mtime() != new_mtime)
          disk_interface_->RemoveFile(inner->path());
      }
      if (!depfile.empty())
        disk_interface_->RemoveFile(depfile);
    }
  }
}

Node* Builder::AddTarget(const std::string& name, std::string* err) {
  Node* node = state_->LookupNode(name);
  if (!node) {
    *err = "unknown target: '" + name + "'";
    return nullptr;
  }
  if (!AddTarget(node, err))
    return nullptr;
  return node;
}

bool Builder::AddTarget(Node* node, std::string* err) {
  if (!scan_.RecomputeDirty(node, err))
    return false;

  if (Edge* in_edge = node->in_edge()) {
    if (in_edge->outputs_ready())
      return true;  // Nothing to do.
  }

  if (!plan_.AddTarget(node, err))
    return false;

  return true;
}

bool Builder::AlreadyUpToDate() const {
  return !plan_.more_to_do();
}

bool Builder::Build(std::string* err) {
  assert(!AlreadyUpToDate());

  status_->PlanHasTotalEdges(plan_.command_edge_count());
  int pending_commands = 0;
  int failures_allowed = config_.failures_allowed;

  // Set up the command runner if we haven't done so already.
  if (!command_runner_.get()) {
    if (config_.dry_run)
      command_runner_.reset(new DryRunCommandRunner);
    else
      command_runner_.reset(new RealCommandRunner(config_));
  }

  // We are about to start the build process.
  status_->BuildStarted();

  // This main loop runs the entire build process.
  // It is structured like this:
  // First, we attempt to start as many commands as allowed by the
  // command runner.
  // Second, we attempt to wait for / reap the next finished command.
  while (plan_.more_to_do()) {
    // See if we can start any more commands.
    if (failures_allowed && command_runner_->CanRunMore()) {
      if (Edge* edge = plan_.FindWork()) {
        if (!StartEdge(edge, err)) {
          Cleanup();
          status_->BuildFinished();
          return false;
        }

        if (edge->is_phony()) {
          plan_.EdgeFinished(edge, Plan::kEdgeSucceeded);
        } else {
          ++pending_commands;
        }

        // We made some progress; go back to the main loop.
        continue;
      }
    }

    // See if we can reap any finished commands.
    if (pending_commands) {
      CommandRunner::Result result;
      if (!command_runner_->WaitForCommand(&result) ||
          result.status == ExitInterrupted) {
        Cleanup();
        status_->BuildFinished();
        *err = "interrupted by user";
        return false;
      }

      --pending_commands;
      if (!FinishCommand(&result, err)) {
        Cleanup();
        status_->BuildFinished();
        return false;
      }

      if (!result.success()) {
        if (failures_allowed)
          failures_allowed--;
      }

      // We made some progress; start the main loop over.
      continue;
    }

    // If we get here, we cannot make any more progress.
    status_->BuildFinished();
    if (failures_allowed == 0) {
      if (config_.failures_allowed > 1)
        *err = "subcommands failed";
      else
        *err = "subcommand failed";
    } else if (failures_allowed < config_.failures_allowed)
      *err = "cannot make progress due to previous errors";
    else
      *err = "stuck [this is a bug]";

    return false;
  }

  status_->BuildFinished();
  return true;
}

bool Builder::StartEdge(Edge* edge, std::string* err) {
  METRIC_RECORD("StartEdge");
  if (edge->is_phony())
    return true;

  status_->BuildEdgeStarted(edge);

  // Create directories necessary for outputs.
  // XXX: this will block; do we care?
  for (const auto & item : edge->outputs_)
  {
    if (!disk_interface_->MakeDirs(item->path()))
      return false;
  }

  // Create response file, if needed
  // XXX: this may also block; do we care?
  std::string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty()) {
    std::string content = edge->GetBinding("rspfile_content");
    if (!disk_interface_->WriteFile(rspfile, content))
      return false;
  }

  // start command computing and run it
  if (!command_runner_->StartCommand(edge)) {
    err->assign("command '" + edge->EvaluateCommand() + "' failed.");
    return false;
  }

  return true;
}

bool Builder::FinishCommand(CommandRunner::Result* result, std::string* err) {
  METRIC_RECORD("FinishCommand");

  Edge* edge = result->edge;

  // First try to extract dependencies from the result, if any.
  // This must happen first as it filters the command output (we want
  // to filter /showIncludes output, even on compile failure) and
  // extraction itself can fail, which makes the command fail from a
  // build perspective.
  std::vector<Node*> deps_nodes;
  std::string deps_type = edge->GetBinding("deps");
  const std::string deps_prefix = edge->GetBinding("msvc_deps_prefix");
  if (!deps_type.empty()) {
    std::string extract_err;
    if (!ExtractDeps(result, deps_type, deps_prefix, &deps_nodes,
                     &extract_err) &&
        result->success()) {
      if (!result->output.empty())
        result->output.append("\n");
      result->output.append(extract_err);
      result->status = ExitFailure;
    }
  }

  int start_time, end_time;
  status_->BuildEdgeFinished(edge, result->success(), result->output,
                             &start_time, &end_time);

  // The rest of this function only applies to successful commands.
  if (!result->success()) {
    plan_.EdgeFinished(edge, Plan::kEdgeFailed);
    return true;
  }

  // Restat the edge outputs
  TimeStamp output_mtime = 0;
  bool restat = edge->GetBindingBool("restat");
  if (!config_.dry_run) {
    bool node_cleaned = false;

    for (const auto & item : edge->outputs_) {
      TimeStamp new_mtime = disk_interface_->Stat(item->path(), err);
      if (new_mtime == -1)
        return false;
      if (new_mtime > output_mtime)
        output_mtime = new_mtime;
      if (item->mtime() == new_mtime && restat) {
        // The rule command did not change the output.  Propagate the clean
        // state through the build graph.
        // Note that this also applies to nonexistent outputs (mtime == 0).
        if (!plan_.CleanNode(&scan_, item, err))
          return false;
        node_cleaned = true;
      }
    }

    if (node_cleaned) {
      TimeStamp restat_mtime = 0;
      // If any output was cleaned, find the most recent mtime of any
      // (existing) non-order-only input or the depfile.
      for (std::vector<Node*>::iterator i = edge->inputs_.begin();
           i != edge->inputs_.end() - edge->order_only_deps_; ++i) {
        TimeStamp input_mtime = disk_interface_->Stat((*i)->path(), err);
        if (input_mtime == -1)
          return false;
        if (input_mtime > restat_mtime)
          restat_mtime = input_mtime;
      }

      std::string depfile = edge->GetUnescapedDepfile();
      if (restat_mtime != 0 && deps_type.empty() && !depfile.empty()) {
        TimeStamp depfile_mtime = disk_interface_->Stat(depfile, err);
        if (depfile_mtime == -1)
          return false;
        if (depfile_mtime > restat_mtime)
          restat_mtime = depfile_mtime;
      }

      // The total number of edges in the plan may have changed as a result
      // of a restat.
      status_->PlanHasTotalEdges(plan_.command_edge_count());

      output_mtime = restat_mtime;
    }
  }

  plan_.EdgeFinished(edge, Plan::kEdgeSucceeded);

  // Delete any left over response file.
  std::string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty() && !g_keep_rsp)
    disk_interface_->RemoveFile(rspfile);

  if (scan_.build_log()) {
    if (!scan_.build_log()->RecordCommand(edge, start_time, end_time,
                                          output_mtime)) {
      *err = std::string("Error writing to build log: ") + strerror(errno);
      return false;
    }
  }

  if (!deps_type.empty() && !config_.dry_run) {
    assert(edge->outputs_.size() == 1 && "should have been rejected by parser");
    Node* out = edge->outputs_[0];
    TimeStamp deps_mtime = disk_interface_->Stat(out->path(), err);
    if (deps_mtime == -1)
      return false;
    if (!scan_.deps_log()->RecordDeps(out, deps_mtime, deps_nodes)) {
      *err = std::string("Error writing to deps log: ") + strerror(errno);
      return false;
    }
  }
  return true;
}

bool Builder::ExtractDeps(CommandRunner::Result* result,
                          const std::string& deps_type,
                          const std::string& deps_prefix,
                          std::vector<Node*>* deps_nodes,
                          std::string* err) {
  if (deps_type == "msvc") {
    CLParser parser;
    std::string output;
    if (!parser.Parse(result->output, deps_prefix, &output, err))
      return false;
    result->output = output;
    for (const auto & item : parser.includes_) {
      // ~0 is assuming that with MSVC-parsed headers, it's ok to always make
      // all backslashes (as some of the slashes will certainly be backslashes
      // anyway). This could be fixed if necessary with some additional
      // complexity in IncludesNormalize::Relativize.
      deps_nodes->push_back(state_->GetNode(item, ~0u));
    }
  } else
  if (deps_type == "gcc") {
    std::string depfile = result->edge->GetUnescapedDepfile();
    if (depfile.empty()) {
      *err = std::string("edge with deps=gcc but no depfile makes no sense");
      return false;
    }

    // Read depfile content.  Treat a missing depfile as empty.
    std::string content;
    switch (disk_interface_->ReadFile(depfile, &content, err)) {
    case DiskInterface::Okay:
      break;
    case DiskInterface::NotFound:
      err->clear();
      break;
    case DiskInterface::OtherError:
      return false;
    }
    if (content.empty())
      return true;

    DepfileParser deps;
    if (!deps.Parse(&content, err))
      return false;

    // XXX check depfile matches expected output.
    deps_nodes->reserve(deps.ins_.size());
    for (auto & item : deps.ins_)
    {
      uint64_t slash_bits;

      size_t size = item.size();
      // What the hell?
      // Who's bright idea was it to use const_cast?
      if (!CanonicalizePath(const_cast<char*>(item.data()),
                            &size,
                            &slash_bits,
                            err))
      {
        item = item.substr(0, size);
        return false;
      }
      item = item.substr(0, size);

      deps_nodes->push_back(state_->GetNode(item, slash_bits));
    }

    if (!g_keep_depfile)
    {
      if (disk_interface_->RemoveFile(depfile) < 0)
      {
        *err = std::string("deleting depfile: ") + strerror(errno) + std::string("\n");
        return false;
      }
    }
  }
  else
  {
    Fatal("unknown deps type '%s'", deps_type.c_str());
  }

  return true;
}
