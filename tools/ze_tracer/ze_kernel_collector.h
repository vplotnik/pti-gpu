//==============================================================
// Copyright (C) Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#ifndef PTI_TOOLS_ZE_TRACER_ZE_KERNEL_COLLECTOR_H_
#define PTI_TOOLS_ZE_TRACER_ZE_KERNEL_COLLECTOR_H_

#include <atomic>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <level_zero/layers/zel_tracing_api.h>

#include "correlator.h"
#include "utils.h"
#include "ze_utils.h"

struct ZeSubmitData {
  uint64_t host_sync;
  uint64_t device_sync;
};

struct ZeKernelGroupSize {
  uint32_t x;
  uint32_t y;
  uint32_t z;
};

struct ZeKernelProps {
  std::string name;
  size_t simd_width;
  size_t bytes_transferred;
  uint32_t group_count[3];
  uint32_t group_size[3];
};

struct ZeKernelCommand {
  ZeKernelProps props;
  ze_event_pool_handle_t event_pool = nullptr;
  ze_event_handle_t event = nullptr;
  ze_device_handle_t device = nullptr;
  uint64_t kernel_id = 0;
  uint64_t append_time = 0;
  uint64_t timer_frequency = 0;
  uint64_t call_count = 0;
};

struct ZeKernelCall {
  ZeKernelCommand* command;
  ze_command_queue_handle_t queue;
  uint64_t submit_time;
  uint64_t device_submit_time;
  uint64_t call_id;
};

struct ZeKernelInfo {
  uint64_t total_time;
  uint64_t min_time;
  uint64_t max_time;
  uint64_t call_count;

  bool operator>(const ZeKernelInfo& r) const {
    if (total_time != r.total_time) {
      return total_time > r.total_time;
    }
    return call_count > r.call_count;
  }

  bool operator!=(const ZeKernelInfo& r) const {
    if (total_time == r.total_time) {
      return call_count != r.call_count;
    }
    return true;
  }
};

struct ZeCommandListInfo {
  std::vector<ZeKernelCommand*> kernel_command_list;
  ze_context_handle_t context;
  ze_device_handle_t device;
  bool immediate;
};

#ifdef PTI_KERNEL_INTERVALS

struct ZeDeviceInterval {
  uint64_t start;
  uint64_t end;
  uint32_t sub_device_id;
};

struct ZeKernelInterval {
  std::string kernel_name;
  ze_device_handle_t device;
  std::vector<ZeDeviceInterval> device_interval_list;
};

using ZeKernelIntervalList = std::vector<ZeKernelInterval>;
using ZeDeviceMap = std::map<
    ze_device_handle_t, std::vector<ze_device_handle_t> >;

#endif // PTI_KERNEL_INTERVALS

using ZeKernelGroupSizeMap = std::map<ze_kernel_handle_t, ZeKernelGroupSize>;
using ZeKernelInfoMap = std::map<std::string, ZeKernelInfo>;
using ZeCommandListMap = std::map<ze_command_list_handle_t, ZeCommandListInfo>;
using ZeImageSizeMap = std::map<ze_image_handle_t, size_t>;

typedef void (*OnZeKernelFinishCallback)(
    void* data, void* queue,
    const std::string& id, const std::string& name,
    uint64_t appended, uint64_t submitted,
    uint64_t started, uint64_t ended);

class ZeKernelCollector {
 public: // Interface

  static ZeKernelCollector* Create(
      Correlator* correlator,
      bool verbose,
      OnZeKernelFinishCallback callback = nullptr,
      void* callback_data = nullptr) {
    PTI_ASSERT(utils::ze::GetVersion() != ZE_API_VERSION_1_0);

    PTI_ASSERT(correlator != nullptr);
    ZeKernelCollector* collector = new ZeKernelCollector(
        correlator, verbose, callback, callback_data);
    PTI_ASSERT(collector != nullptr);

    ze_result_t status = ZE_RESULT_SUCCESS;
    zel_tracer_desc_t tracer_desc = {
        ZEL_STRUCTURE_TYPE_TRACER_EXP_DESC, nullptr, collector};
    zel_tracer_handle_t tracer = nullptr;
    status = zelTracerCreate(&tracer_desc, &tracer);
    if (status != ZE_RESULT_SUCCESS) {
      std::cerr << "[WARNING] Unable to create Level Zero tracer" << std::endl;
      delete collector;
      return nullptr;
    }

    collector->EnableTracing(tracer);
    return collector;
  }

  ~ZeKernelCollector() {
    if (tracer_ != nullptr) {
      ze_result_t status = zelTracerDestroy(tracer_);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    }
  }

  void PrintKernelsTable() const {
    std::set< std::pair<std::string, ZeKernelInfo>,
              utils::Comparator > sorted_list(
        kernel_info_map_.begin(), kernel_info_map_.end());

    uint64_t total_duration = 0;
    size_t max_name_length = kKernelLength;
    for (auto& value : sorted_list) {
      total_duration += value.second.total_time;
      if (value.first.size() > max_name_length) {
        max_name_length = value.first.size();
      }
    }

    if (total_duration == 0) {
      return;
    }

    std::stringstream stream;
    stream << std::setw(max_name_length) << "Kernel" << "," <<
      std::setw(kCallsLength) << "Calls" << "," <<
      std::setw(kTimeLength) << "Time (ns)" << "," <<
      std::setw(kPercentLength) << "Time (%)" << "," <<
      std::setw(kTimeLength) << "Average (ns)" << "," <<
      std::setw(kTimeLength) << "Min (ns)" << "," <<
      std::setw(kTimeLength) << "Max (ns)" << std::endl;

    for (auto& value : sorted_list) {
      const std::string& function = value.first;
      uint64_t call_count = value.second.call_count;
      uint64_t duration = value.second.total_time;
      uint64_t avg_duration = duration / call_count;
      uint64_t min_duration = value.second.min_time;
      uint64_t max_duration = value.second.max_time;
      float percent_duration = 100.0f * duration / total_duration;
      stream << std::setw(max_name_length) << function << "," <<
        std::setw(kCallsLength) << call_count << "," <<
        std::setw(kTimeLength) << duration << "," <<
        std::setw(kPercentLength) << std::setprecision(2) <<
          std::fixed << percent_duration << "," <<
        std::setw(kTimeLength) << avg_duration << "," <<
        std::setw(kTimeLength) << min_duration << "," <<
        std::setw(kTimeLength) << max_duration << std::endl;
    }

    PTI_ASSERT(correlator_ != nullptr);
    correlator_->Log(stream.str());
  }

  void DisableTracing() {
    PTI_ASSERT(tracer_ != nullptr);
    ze_result_t status = ZE_RESULT_SUCCESS;
    status = zelTracerSetEnabled(tracer_, false);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  }

  const ZeKernelInfoMap& GetKernelInfoMap() const {
    return kernel_info_map_;
  }

#ifdef PTI_KERNEL_INTERVALS
  const ZeKernelIntervalList& GetKernelIntervalList() const {
    return kernel_interval_list_;
  }
#endif // PTI_KERNEL_INTERVALS

 private: // Implementation

  ZeKernelCollector(
      Correlator* correlator,
      bool verbose,
      OnZeKernelFinishCallback callback,
      void* callback_data)
      : correlator_(correlator),
        verbose_(verbose),
        callback_(callback),
        callback_data_(callback_data),
        kernel_id_(1) {
    PTI_ASSERT(correlator_ != nullptr);
#ifdef PTI_KERNEL_INTERVALS
    CreateDeviceMap();
#endif // PTI_KERNEL_INTERVALS
  }

#ifdef PTI_KERNEL_INTERVALS
  void CreateDeviceMap() {
    std::vector<ze_device_handle_t> device_list =
      utils::ze::GetDeviceList();
    for (auto device : device_list) {
      std::vector<ze_device_handle_t> sub_device_list =
        utils::ze::GetSubDeviceList(device);
      PTI_ASSERT(device_map_.count(device) == 0);
      device_map_[device] = sub_device_list;
    }
  }
#endif // PTI_KERNEL_INTERVALS

  uint64_t GetHostTimestamp() const {
    PTI_ASSERT(correlator_ != nullptr);
    return correlator_->GetTimestamp();
  }

  uint64_t GetDeviceTimestamp(ze_device_handle_t device) const {
    PTI_ASSERT(device != nullptr);
    return utils::ze::GetDeviceTimestamp(device) &
      utils::ze::GetDeviceTimestampMask(device);
  }

  void EnableTracing(zel_tracer_handle_t tracer) {
    PTI_ASSERT(tracer != nullptr);
    tracer_ = tracer;

    zet_core_callbacks_t prologue_callbacks{};
    zet_core_callbacks_t epilogue_callbacks{};

    prologue_callbacks.Event.pfnDestroyCb = OnEnterEventDestroy;

    prologue_callbacks.Event.pfnHostResetCb = OnEnterEventHostReset;

    prologue_callbacks.EventPool.pfnCreateCb = OnEnterEventPoolCreate;
    epilogue_callbacks.EventPool.pfnCreateCb = OnExitEventPoolCreate;

    prologue_callbacks.CommandList.pfnAppendLaunchKernelCb =
      OnEnterCommandListAppendLaunchKernel;
    epilogue_callbacks.CommandList.pfnAppendLaunchKernelCb =
      OnExitCommandListAppendLaunchKernel;

    prologue_callbacks.CommandList.pfnAppendLaunchCooperativeKernelCb =
      OnEnterCommandListAppendLaunchCooperativeKernel;
    epilogue_callbacks.CommandList.pfnAppendLaunchCooperativeKernelCb =
      OnExitCommandListAppendLaunchCooperativeKernel;

    prologue_callbacks.CommandList.pfnAppendLaunchKernelIndirectCb =
      OnEnterCommandListAppendLaunchKernelIndirect;
    epilogue_callbacks.CommandList.pfnAppendLaunchKernelIndirectCb =
      OnExitCommandListAppendLaunchKernelIndirect;

    prologue_callbacks.CommandList.pfnAppendMemoryCopyCb =
      OnEnterCommandListAppendMemoryCopy;
    epilogue_callbacks.CommandList.pfnAppendMemoryCopyCb =
      OnExitCommandListAppendMemoryCopy;

    prologue_callbacks.CommandList.pfnAppendMemoryFillCb =
      OnEnterCommandListAppendMemoryFill;
    epilogue_callbacks.CommandList.pfnAppendMemoryFillCb =
      OnExitCommandListAppendMemoryFill;

    prologue_callbacks.CommandList.pfnAppendBarrierCb =
      OnEnterCommandListAppendBarrier;
    epilogue_callbacks.CommandList.pfnAppendBarrierCb =
      OnExitCommandListAppendBarrier;

    prologue_callbacks.CommandList.pfnAppendMemoryRangesBarrierCb =
      OnEnterCommandListAppendMemoryRangesBarrier;
    epilogue_callbacks.CommandList.pfnAppendMemoryRangesBarrierCb =
      OnExitCommandListAppendMemoryRangesBarrier;

    prologue_callbacks.CommandList.pfnAppendMemoryCopyRegionCb =
      OnEnterCommandListAppendMemoryCopyRegion;
    epilogue_callbacks.CommandList.pfnAppendMemoryCopyRegionCb =
      OnExitCommandListAppendMemoryCopyRegion;

    prologue_callbacks.CommandList.pfnAppendMemoryCopyFromContextCb =
      OnEnterCommandListAppendMemoryCopyFromContext;
    epilogue_callbacks.CommandList.pfnAppendMemoryCopyFromContextCb =
      OnExitCommandListAppendMemoryCopyFromContext;

    prologue_callbacks.CommandList.pfnAppendImageCopyCb =
      OnEnterCommandListAppendImageCopy;
    epilogue_callbacks.CommandList.pfnAppendImageCopyCb =
      OnExitCommandListAppendImageCopy;

    prologue_callbacks.CommandList.pfnAppendImageCopyRegionCb =
      OnEnterCommandListAppendImageCopyRegion;
    epilogue_callbacks.CommandList.pfnAppendImageCopyRegionCb =
      OnExitCommandListAppendImageCopyRegion;

    prologue_callbacks.CommandList.pfnAppendImageCopyToMemoryCb =
      OnEnterCommandListAppendImageCopyToMemory;
    epilogue_callbacks.CommandList.pfnAppendImageCopyToMemoryCb =
      OnExitCommandListAppendImageCopyToMemory;

    prologue_callbacks.CommandList.pfnAppendImageCopyFromMemoryCb =
      OnEnterCommandListAppendImageCopyFromMemory;
    epilogue_callbacks.CommandList.pfnAppendImageCopyFromMemoryCb =
      OnExitCommandListAppendImageCopyFromMemory;

    prologue_callbacks.CommandQueue.pfnExecuteCommandListsCb =
      OnEnterCommandQueueExecuteCommandLists;
    epilogue_callbacks.CommandQueue.pfnExecuteCommandListsCb =
      OnExitCommandQueueExecuteCommandLists;

    epilogue_callbacks.CommandList.pfnCreateCb =
      OnExitCommandListCreate;
    epilogue_callbacks.CommandList.pfnCreateImmediateCb =
      OnExitCommandListCreateImmediate;
    epilogue_callbacks.CommandList.pfnDestroyCb =
      OnExitCommandListDestroy;
    epilogue_callbacks.CommandList.pfnResetCb =
      OnExitCommandListReset;

    epilogue_callbacks.CommandQueue.pfnSynchronizeCb =
      OnExitCommandQueueSynchronize;
    epilogue_callbacks.CommandQueue.pfnDestroyCb =
      OnExitCommandQueueDestroy;

    epilogue_callbacks.Image.pfnCreateCb =
      OnExitImageCreate;
    epilogue_callbacks.Image.pfnDestroyCb =
      OnExitImageDestroy;

    epilogue_callbacks.Kernel.pfnSetGroupSizeCb =
      OnExitKernelSetGroupSize;
    epilogue_callbacks.Kernel.pfnDestroyCb =
      OnExitKernelDestroy;

    epilogue_callbacks.Event.pfnHostSynchronizeCb =
      OnExitEventHostSynchronize;

    ze_result_t status = ZE_RESULT_SUCCESS;
    status = zelTracerSetPrologues(tracer_, &prologue_callbacks);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    status = zelTracerSetEpilogues(tracer_, &epilogue_callbacks);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
    status = zelTracerSetEnabled(tracer_, true);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  }

  void AddKernelCommand(
      ze_command_list_handle_t command_list, ZeKernelCommand* command) {
    PTI_ASSERT(command_list != nullptr);
    PTI_ASSERT(command != nullptr);

    const std::lock_guard<std::mutex> lock(lock_);

    command->kernel_id =
      kernel_id_.fetch_add(1, std::memory_order::memory_order_relaxed);
    PTI_ASSERT(correlator_ != nullptr);
    correlator_->SetKernelId(command->kernel_id);
    correlator_->AddKernelId(command_list, command->kernel_id);

    PTI_ASSERT(command_list_map_.count(command_list) == 1);
    ZeCommandListInfo& command_list_info = command_list_map_[command_list];
    command_list_info.kernel_command_list.push_back(command);
  }

  void AddKernelCall(
      ze_command_list_handle_t command_list, ZeKernelCall* call) {
    PTI_ASSERT(command_list != nullptr);
    PTI_ASSERT(call != nullptr);

    const std::lock_guard<std::mutex> lock(lock_);

    ZeKernelCommand* command = call->command;
    PTI_ASSERT(command != nullptr);
    ++(command->call_count);
    call->call_id = command->call_count;

    kernel_call_list_.push_back(call);

    PTI_ASSERT(correlator_ != nullptr);
    correlator_->AddCallId(command_list, call->call_id);
  }

  void ProcessCall(ze_event_handle_t event) {
    PTI_ASSERT(event != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);

    ze_result_t status = ZE_RESULT_SUCCESS;
    status = zeEventQueryStatus(event);
    if (status != ZE_RESULT_SUCCESS) {
      return;
    }

    for (auto it = kernel_call_list_.begin();
         it != kernel_call_list_.end(); ++it) {
      ZeKernelCall* call = *it;
      PTI_ASSERT(call != nullptr);
      ZeKernelCommand* command = call->command;
      PTI_ASSERT(command != nullptr);

      if (command->event == event) {
        ProcessCall(call);
        kernel_call_list_.erase(it);
        break;
      }
    }
  }

  void ProcessCall(const ZeKernelCall* call) {
    PTI_ASSERT(call != nullptr);
    ZeKernelCommand* command = call->command;
    PTI_ASSERT(command != nullptr);

    ze_result_t status = ZE_RESULT_SUCCESS;
    status = zeEventQueryStatus(command->event);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    ze_kernel_timestamp_result_t timestamp{};
    status = zeEventQueryKernelTimestamp(command->event, &timestamp);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    uint64_t start = timestamp.global.kernelStart;
    uint64_t end = timestamp.global.kernelEnd;
    uint64_t freq = command->timer_frequency;
    PTI_ASSERT(freq > 0);

    uint64_t duration = 0;
    if (start < end) {
      duration = (end - start) *
        static_cast<uint64_t>(NSEC_IN_SEC) / freq;
    } else { // 32-bit timer overflow
      duration = ((1ull << 32) + end - start) *
        static_cast<uint64_t>(NSEC_IN_SEC) / freq;
    }

    PTI_ASSERT(call->submit_time > 0);
    PTI_ASSERT(call->device_submit_time > 0);
    PTI_ASSERT(start > call->device_submit_time);
    uint64_t time_shift = (start - call->device_submit_time) *
      static_cast<uint64_t>(NSEC_IN_SEC) / freq;
    uint64_t host_start = call->submit_time + time_shift;
    uint64_t host_end = host_start + duration;

    AddKernelInfo(host_end - host_start, &command->props);
#ifdef PTI_KERNEL_INTERVALS
    AddKernelInterval(command);
#endif // PTI_KERNEL_INTERVALS

    if (callback_ != nullptr) {
      PTI_ASSERT(command->append_time > 0);
      PTI_ASSERT(command->append_time <= call->submit_time);

      PTI_ASSERT(call->queue != nullptr);
      PTI_ASSERT(!command->props.name.empty());
      std::string id = std::to_string(command->kernel_id) + "." +
        std::to_string(call->call_id);
      callback_(
          callback_data_, call->queue,
          id, command->props.name,
          command->append_time, call->submit_time,
          host_start, host_end);
    }

    delete call;
  }

  void ProcessCalls() {
    ze_result_t status = ZE_RESULT_SUCCESS;
    const std::lock_guard<std::mutex> lock(lock_);

    auto it = kernel_call_list_.begin();
    while (it != kernel_call_list_.end()) {
      ZeKernelCall* call = *it;
      PTI_ASSERT(call != nullptr);
      ZeKernelCommand* command = call->command;
      PTI_ASSERT(command != nullptr);

      PTI_ASSERT(command->event != nullptr);
      status = zeEventQueryStatus(command->event);
      if (status == ZE_RESULT_NOT_READY) {
        ++it;
      } else if (status == ZE_RESULT_SUCCESS) {
        ProcessCall(call);
        it = kernel_call_list_.erase(it);
      } else {
        PTI_ASSERT(0);
      }
    }
  }

  static std::string GetVerboseName(
      const std::string& name, const ZeKernelProps* props) {
    PTI_ASSERT(!name.empty());
    PTI_ASSERT(props != nullptr);

    std::stringstream sstream;
    sstream << name;
    if (props->simd_width > 0) {
      sstream << "[SIMD" << props->simd_width << " {" <<
        props->group_count[0] << "; " <<
        props->group_count[1] << "; " <<
        props->group_count[2] << "} {" <<
        props->group_size[0] << "; " <<
        props->group_size[1] << "; " <<
        props->group_size[2] << "}]";
    } else if (props->bytes_transferred > 0) {
      sstream << "[" << props->bytes_transferred << " bytes]";
    }

    return sstream.str();
  }

  void AddKernelInfo(uint64_t time, const ZeKernelProps* props) {
    PTI_ASSERT(props != nullptr);

    std::string name = props->name;
    PTI_ASSERT(!name.empty());

    if (verbose_) {
      name = GetVerboseName(name, props);
    }

    if (kernel_info_map_.count(name) == 0) {
      kernel_info_map_[name] = {time, time, time, 1};
    } else {
      ZeKernelInfo& kernel = kernel_info_map_[name];
      kernel.total_time += time;
      if (time > kernel.max_time) {
        kernel.max_time = time;
      }
      if (time < kernel.min_time) {
        kernel.min_time = time;
      }
      kernel.call_count += 1;
    }
  }

#ifdef PTI_KERNEL_INTERVALS
  void AddKernelInterval(const ZeKernelCommand* command) {
    PTI_ASSERT(command != nullptr);

    std::string name = command->props.name;
    PTI_ASSERT(!name.empty());

    if (verbose_) {
      name = GetVerboseName(name, &command->props);
    }

    ze_result_t status = zeEventQueryStatus(command->event);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    if (device_map_.count(command->device) == 1 &&
        !device_map_[command->device].empty()) { // Implicit Scaling
      // TODO: Use zeEventQueryTimestampsExp for better results
      ze_kernel_timestamp_result_t timestamp{};
      status = zeEventQueryKernelTimestamp(command->event, &timestamp);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      uint64_t start = timestamp.global.kernelStart;
      uint64_t end = timestamp.global.kernelEnd;
      uint64_t freq = command->timer_frequency;
      PTI_ASSERT(freq > 0);

      uint64_t duration = 0;
      if (start < end) {
        duration = (end - start) *
          static_cast<uint64_t>(NSEC_IN_SEC) / freq;
      } else { // 32-bit timer overflow
        duration = ((1ull << 32) + end - start) *
          static_cast<uint64_t>(NSEC_IN_SEC) / freq;
      }

      uint64_t start_ns = start *
        static_cast<uint64_t>(NSEC_IN_SEC) / freq;
      uint64_t end_ns = start_ns + duration;
      PTI_ASSERT(start_ns < end_ns);

      ZeKernelInterval kernel_interval{
          name, command->device, std::vector<ZeDeviceInterval>()};
      for (size_t i = 0; i < device_map_[command->device].size(); ++i) {
        PTI_ASSERT(i < (std::numeric_limits<uint32_t>::max)());
        kernel_interval.device_interval_list.push_back(
            {start_ns, end_ns, static_cast<uint32_t>(i)});
      }
      kernel_interval_list_.push_back(kernel_interval);
    } else { // Explicit scaling
      ze_kernel_timestamp_result_t timestamp{};
      status = zeEventQueryKernelTimestamp(command->event, &timestamp);
      PTI_ASSERT(status == ZE_RESULT_SUCCESS);

      uint64_t start = timestamp.global.kernelStart;
      uint64_t end = timestamp.global.kernelEnd;
      uint64_t freq = command->timer_frequency;
      PTI_ASSERT(freq > 0);

      uint64_t duration = 0;
      if (start < end) {
        duration = (end - start) *
          static_cast<uint64_t>(NSEC_IN_SEC) / freq;
      } else { // 32-bit timer overflow
        duration = ((1ull << 32) + end - start) *
          static_cast<uint64_t>(NSEC_IN_SEC) / freq;
      }

      uint64_t start_ns = start *
        static_cast<uint64_t>(NSEC_IN_SEC) / freq;
      uint64_t end_ns = start_ns + duration;
      PTI_ASSERT(start_ns < end_ns);

      if (device_map_.count(command->device) == 0) { // Subdevice
        for (auto it : device_map_) {
          std::vector<ze_device_handle_t> sub_device_list = it.second;
          for (size_t i = 0; i < sub_device_list.size(); ++i) {
            if (sub_device_list[i] == command->device) {
              ZeKernelInterval kernel_interval{
                  name, it.first, std::vector<ZeDeviceInterval>()};
              PTI_ASSERT(i < (std::numeric_limits<uint32_t>::max)());
              kernel_interval.device_interval_list.push_back(
                  {start_ns, end_ns, static_cast<uint32_t>(i)});
              kernel_interval_list_.push_back(kernel_interval);
              return;
            }
          }
        }
        PTI_ASSERT(0);
      } else { // Device with no subdevices
        PTI_ASSERT(device_map_[command->device].empty());
        ZeKernelInterval kernel_interval{
            name, command->device, std::vector<ZeDeviceInterval>()};
        kernel_interval.device_interval_list.push_back({start_ns, end_ns, 0});
        kernel_interval_list_.push_back(kernel_interval);
      }
    }
  }
#endif // PTI_KERNEL_INTERVALS

  void AddCommandList(
      ze_command_list_handle_t command_list,
      ze_context_handle_t context,
      ze_device_handle_t device,
      bool immediate) {
    PTI_ASSERT(command_list != nullptr);
    PTI_ASSERT(context != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    PTI_ASSERT(command_list_map_.count(command_list) == 0);
    command_list_map_[command_list] =
      {std::vector<ZeKernelCommand*>(), context, device, immediate};

    PTI_ASSERT(correlator_ != nullptr);
    correlator_->CreateKernelIdList(command_list);
    correlator_->CreateCallIdList(command_list);
  }

  void RemoveKernelCommands(ze_command_list_handle_t command_list) {
    PTI_ASSERT(command_list != nullptr);

    PTI_ASSERT(command_list_map_.count(command_list) == 1);
    ZeCommandListInfo& info = command_list_map_[command_list];
    for (ZeKernelCommand* command : info.kernel_command_list) {
      if (command->event_pool != nullptr) {
        ze_result_t status = ZE_RESULT_SUCCESS;
        status = zeEventDestroy(command->event);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        status = zeEventPoolDestroy(command->event_pool);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      }

      for (ZeKernelCall* call : kernel_call_list_) {
        PTI_ASSERT(call->command != command);
      }

      delete command;
    }
    info.kernel_command_list.clear();
  }

  void RemoveCommandList(ze_command_list_handle_t command_list) {
    PTI_ASSERT(command_list != nullptr);

    const std::lock_guard<std::mutex> lock(lock_);

    RemoveKernelCommands(command_list);
    command_list_map_.erase(command_list);

    PTI_ASSERT(correlator_ != nullptr);
    correlator_->RemoveKernelIdList(command_list);
    correlator_->RemoveCallIdList(command_list);
  }

  void ResetCommandList(ze_command_list_handle_t command_list) {
    PTI_ASSERT(command_list != nullptr);

    const std::lock_guard<std::mutex> lock(lock_);

    RemoveKernelCommands(command_list);

    PTI_ASSERT(correlator_ != nullptr);
    correlator_->ResetKernelIdList(command_list);
    correlator_->ResetCallIdList(command_list);
  }

  void AddKernelCalls(
      ze_command_list_handle_t command_list,
      ze_command_queue_handle_t queue, const ZeSubmitData* submit_data) {
    PTI_ASSERT(command_list != nullptr);

    const std::lock_guard<std::mutex> lock(lock_);

    PTI_ASSERT(command_list_map_.count(command_list) == 1);
    ZeCommandListInfo& info = command_list_map_[command_list];
    PTI_ASSERT(!info.immediate);

    PTI_ASSERT(correlator_ != nullptr);
    correlator_->ResetCallIdList(command_list);

    for (ZeKernelCommand* command : info.kernel_command_list) {
      ZeKernelCall* call = new ZeKernelCall;
      PTI_ASSERT(call != nullptr);

      call->command = command;
      call->queue = queue;
      call->submit_time = submit_data->host_sync;
      call->device_submit_time = submit_data->device_sync;
      PTI_ASSERT(command->append_time <= call->submit_time);
      ++(command->call_count);
      call->call_id = command->call_count;

      kernel_call_list_.push_back(call);
      correlator_->AddCallId(command_list, call->call_id);
    }
  }

  ze_context_handle_t GetCommandListContext(
      ze_command_list_handle_t command_list) {
    PTI_ASSERT(command_list != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    PTI_ASSERT(command_list_map_.count(command_list) == 1);
    ZeCommandListInfo& command_list_info = command_list_map_[command_list];
    return command_list_info.context;
  }

  ze_device_handle_t GetCommandListDevice(
      ze_command_list_handle_t command_list) {
    PTI_ASSERT(command_list != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    PTI_ASSERT(command_list_map_.count(command_list) == 1);
    ZeCommandListInfo& command_list_info = command_list_map_[command_list];
    return command_list_info.device;
  }

  bool IsCommandListImmediate(ze_command_list_handle_t command_list) {
    PTI_ASSERT(command_list != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    PTI_ASSERT(command_list_map_.count(command_list) == 1);
    ZeCommandListInfo& command_list_info = command_list_map_[command_list];
    return command_list_info.immediate;
  }

  void AddImage(ze_image_handle_t image, size_t size) {
    PTI_ASSERT(image != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    PTI_ASSERT(image_size_map_.count(image) == 0);
    image_size_map_[image] = size;
  }

  void RemoveImage(ze_image_handle_t image) {
    PTI_ASSERT(image != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    PTI_ASSERT(image_size_map_.count(image) == 1);
    image_size_map_.erase(image);
  }

  size_t GetImageSize(ze_image_handle_t image) {
    PTI_ASSERT(image != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    if (image_size_map_.count(image) == 1) {
      return image_size_map_[image];
    }
    return 0;
  }

  void AddKernelGroupSize(
      ze_kernel_handle_t kernel, const ZeKernelGroupSize& group_size) {
    PTI_ASSERT(kernel != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    kernel_group_size_map_[kernel] = group_size;
  }

  void RemoveKernelGroupSize(ze_kernel_handle_t kernel) {
    PTI_ASSERT(kernel != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    kernel_group_size_map_.erase(kernel);
  }

  ZeKernelGroupSize GetKernelGroupSize(ze_kernel_handle_t kernel) {
    PTI_ASSERT(kernel != nullptr);
    const std::lock_guard<std::mutex> lock(lock_);
    if (kernel_group_size_map_.count(kernel) == 0) {
      return {0, 0, 0};
    }
    return kernel_group_size_map_[kernel];
  }

 private: // Callbacks

  static void OnEnterEventPoolCreate(ze_event_pool_create_params_t *params,
                                     ze_result_t result,
                                     void *global_data,
                                     void **instance_data) {
    const ze_event_pool_desc_t* desc = *(params->pdesc);
    if (desc == nullptr) {
      return;
    }
    if (desc->flags & ZE_EVENT_POOL_FLAG_IPC) {
      return;
    }

    ze_event_pool_desc_t* profiling_desc = new ze_event_pool_desc_t;
    PTI_ASSERT(profiling_desc != nullptr);
    profiling_desc->stype = desc->stype;
    // PTI_ASSERT(profiling_desc->stype == ZE_STRUCTURE_TYPE_EVENT_POOL_DESC);
    profiling_desc->pNext = desc->pNext;
    profiling_desc->flags = desc->flags;
    profiling_desc->flags |= ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP;
    profiling_desc->flags |= ZE_EVENT_POOL_FLAG_HOST_VISIBLE;
    profiling_desc->count = desc->count;

    *(params->pdesc) = profiling_desc;
    *instance_data = profiling_desc;
  }

  static void OnExitEventPoolCreate(ze_event_pool_create_params_t *params,
                                    ze_result_t result,
                                    void *global_data,
                                    void **instance_data) {
    ze_event_pool_desc_t* desc =
      static_cast<ze_event_pool_desc_t*>(*instance_data);
    if (desc != nullptr) {
      delete desc;
    }
  }

  static void OnEnterEventDestroy(
      ze_event_destroy_params_t *params,
      ze_result_t result, void *global_data, void **instance_data) {
    if (*(params->phEvent) != nullptr) {
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->ProcessCall(*(params->phEvent));
    }
  }

  static void OnEnterEventHostReset(
      ze_event_host_reset_params_t *params, ze_result_t result,
      void *global_data, void **instance_data) {
    if (*(params->phEvent) != nullptr) {
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->ProcessCall(*(params->phEvent));
    }
  }

  static void OnExitEventHostSynchronize(
      ze_event_host_synchronize_params_t *params,
      ze_result_t result, void *global_data, void **instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*(params->phEvent) != nullptr);
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->ProcessCall(*(params->phEvent));
    }
  }

  static void OnExitImageCreate(
      ze_image_create_params_t *params, ze_result_t result,
      void *global_data, void **instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeKernelCollector* collector =
          reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);

      ze_image_desc_t image_desc = **(params->pdesc);
      size_t image_size = image_desc.width;
      switch(image_desc.type) {
        case ZE_IMAGE_TYPE_2D:
        case ZE_IMAGE_TYPE_2DARRAY:
          image_size *= image_desc.height;
          break;
        case ZE_IMAGE_TYPE_3D:
          image_size *= image_desc.height * image_desc.depth;
          break;
      }

      switch(image_desc.format.type) {
        case ZE_IMAGE_FORMAT_TYPE_UINT:
        case ZE_IMAGE_FORMAT_TYPE_UNORM:
        case ZE_IMAGE_FORMAT_TYPE_FORCE_UINT32:
          image_size *= sizeof(unsigned int);
          break;
        case ZE_IMAGE_FORMAT_TYPE_SINT:
        case ZE_IMAGE_FORMAT_TYPE_SNORM:
          image_size *= sizeof(int);
          break;
        case ZE_IMAGE_FORMAT_TYPE_FLOAT:
          image_size *= sizeof(float);
          break;
      }

      collector->AddImage(**(params->pphImage), image_size);
    }
  }

  static void OnExitImageDestroy(
      ze_image_destroy_params_t *params, ze_result_t result,
      void *global_data, void **instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->RemoveImage(*(params->phImage));
    }
  }

  static void CreateEvent(ze_context_handle_t context,
                          ze_event_pool_handle_t& event_pool,
                          ze_event_handle_t& event) {
    PTI_ASSERT(context != nullptr);
    ze_result_t status = ZE_RESULT_SUCCESS;

    ze_event_pool_desc_t event_pool_desc = {
        ZE_STRUCTURE_TYPE_EVENT_POOL_DESC, nullptr,
        ZE_EVENT_POOL_FLAG_KERNEL_TIMESTAMP | ZE_EVENT_POOL_FLAG_HOST_VISIBLE,
        1};
    status = zeEventPoolCreate(
        context, &event_pool_desc, 0, nullptr, &event_pool);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);

    ze_event_desc_t event_desc = {
        ZE_STRUCTURE_TYPE_EVENT_DESC, nullptr, 0,
        ZE_EVENT_SCOPE_FLAG_HOST, ZE_EVENT_SCOPE_FLAG_HOST};
    zeEventCreate(event_pool, &event_desc, &event);
    PTI_ASSERT(status == ZE_RESULT_SUCCESS);
  }

  static void OnEnterKernelAppend(
      const ZeKernelProps& props,
      ze_event_handle_t& signal_event,
      ze_command_list_handle_t command_list,
      void* global_data,
      void** instance_data) {
    ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
    PTI_ASSERT(collector != nullptr);

    if (command_list == nullptr) {
      return;
    }

    ZeKernelCommand* command = new ZeKernelCommand;
    PTI_ASSERT(command != nullptr);
    command->props = props;
    command->append_time = collector->GetHostTimestamp();

    ze_device_handle_t device = collector->GetCommandListDevice(command_list);
    PTI_ASSERT(device != nullptr);
    command->device = device;
    command->timer_frequency = utils::ze::GetDeviceTimerFrequency(device);
    PTI_ASSERT(command->timer_frequency > 0);

    if (signal_event == nullptr) {
      ze_context_handle_t context =
        collector->GetCommandListContext(command_list);
      CreateEvent(context, command->event_pool, command->event);
      signal_event = command->event;
    } else {
      command->event_pool = nullptr;
      command->event = signal_event;
    }

    ZeKernelCall* call = new ZeKernelCall{nullptr, nullptr, 0, 0};
    PTI_ASSERT(call != nullptr);
    call->command = command;

    if (collector->IsCommandListImmediate(command_list)) {
      call->submit_time = command->append_time;
      call->device_submit_time = collector->GetDeviceTimestamp(device);
      call->queue = reinterpret_cast<ze_command_queue_handle_t>(command_list);
    }

    *instance_data = static_cast<void*>(call);
  }

  static void OnExitKernelAppend(
      ze_command_list_handle_t command_list,
      void* global_data, void** instance_data,
      ze_result_t result) {
    PTI_ASSERT(command_list != nullptr);

    ZeKernelCall* call = static_cast<ZeKernelCall*>(*instance_data);
    if (call == nullptr) {
      return;
    }

    ZeKernelCommand* command = call->command;
    PTI_ASSERT(command != nullptr);

    if (result != ZE_RESULT_SUCCESS) {
      if (command->event_pool != nullptr) {
        ze_result_t status = ZE_RESULT_SUCCESS;
        status = zeEventDestroy(command->event);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
        status = zeEventPoolDestroy(command->event_pool);
        PTI_ASSERT(status == ZE_RESULT_SUCCESS);
      }

      delete call;
      delete command;
    } else {
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->AddKernelCommand(command_list, command);
      if (call->queue != nullptr) {
        collector->AddKernelCall(command_list, call);
      } else {
        delete call;
      }
    }
  }

  static ZeKernelProps GetKernelProps(
      ze_kernel_handle_t kernel,
      const ze_group_count_t* group_count,
      void* global_data) {
    PTI_ASSERT(kernel != nullptr);

    ZeKernelProps props{};

    props.name = utils::ze::GetKernelName(kernel);
    props.simd_width =
      utils::ze::GetKernelMaxSubgroupSize(kernel);
    props.bytes_transferred = 0;

    ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
    PTI_ASSERT(collector != nullptr);
    ZeKernelGroupSize group_size = collector->GetKernelGroupSize(kernel);

    props.group_size[0] = group_size.x;
    props.group_size[1] = group_size.y;
    props.group_size[2] = group_size.z;

    if (group_count != nullptr) {
      props.group_count[0] = group_count->groupCountX;
      props.group_count[1] = group_count->groupCountY;
      props.group_count[2] = group_count->groupCountZ;
    }

    return props;
  }

  static ZeKernelProps GetTransferProps(
      std::string name, size_t bytes_transferred) {
    ZeKernelProps props{};
    props.name = name;
    props.bytes_transferred = bytes_transferred;
    return props;
  }

  static void OnEnterCommandListAppendLaunchKernel(
      ze_command_list_append_launch_kernel_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    OnEnterKernelAppend(
        GetKernelProps(
            *(params->phKernel),
            *(params->ppLaunchFuncArgs),
            global_data),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendLaunchCooperativeKernel(
      ze_command_list_append_launch_cooperative_kernel_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    OnEnterKernelAppend(
        GetKernelProps(
            *(params->phKernel),
            *(params->ppLaunchFuncArgs),
            global_data),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendLaunchKernelIndirect(
      ze_command_list_append_launch_kernel_indirect_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    OnEnterKernelAppend(
        GetKernelProps(
            *(params->phKernel),
            *(params->ppLaunchArgumentsBuffer),
            global_data),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendMemoryCopy(
      ze_command_list_append_memory_copy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    OnEnterKernelAppend(
        GetTransferProps("zeCommandListAppendMemoryCopy", *(params->psize)),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendMemoryFill(
      ze_command_list_append_memory_fill_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    OnEnterKernelAppend(
        GetTransferProps("zeCommandListAppendMemoryFill", *(params->psize)),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendMemoryCopyFromContext(
      ze_command_list_append_memory_copy_from_context_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    OnEnterKernelAppend(
        GetTransferProps(
            "zeCommandListAppendMemoryCopyFromContext", *(params->psize)),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendBarrier(
      ze_command_list_append_barrier_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    OnEnterKernelAppend(
        GetTransferProps("zeCommandListAppendBarrier", 0),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendMemoryRangesBarrier(
      ze_command_list_append_memory_ranges_barrier_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    OnEnterKernelAppend(
        GetTransferProps("zeCommandListAppendMemoryRangesBarrier", 0),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendMemoryCopyRegion(
      ze_command_list_append_memory_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    size_t bytes_transferred = 0;
    const ze_copy_region_t* region = *(params->psrcRegion);

    if (region != nullptr) {
      size_t bytes_transferred =
        region->width * region->height * (*(params->psrcPitch));
      if (region->depth != 0) {
        bytes_transferred *= region->depth;
      }
    }

    OnEnterKernelAppend(
        GetTransferProps(
            "zeCommandListAppendMemoryCopyRegion", bytes_transferred),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendImageCopy(
      ze_command_list_append_image_copy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
    PTI_ASSERT(collector != nullptr);
    size_t bytes_transferred = collector->GetImageSize(*(params->phSrcImage));

    OnEnterKernelAppend(
        GetTransferProps("zeCommandListAppendImageCopy", bytes_transferred),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendImageCopyRegion(
      ze_command_list_append_image_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
    PTI_ASSERT(collector != nullptr);
    size_t bytes_transferred = collector->GetImageSize(*(params->phSrcImage));

    OnEnterKernelAppend(
        GetTransferProps(
            "zeCommandListAppendImageCopyRegion", bytes_transferred),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendImageCopyToMemory(
      ze_command_list_append_image_copy_to_memory_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
    PTI_ASSERT(collector != nullptr);
    size_t bytes_transferred = collector->GetImageSize(*(params->phSrcImage));

    OnEnterKernelAppend(
        GetTransferProps(
            "zeCommandListAppendImageCopyToMemory", bytes_transferred),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnEnterCommandListAppendImageCopyFromMemory(
      ze_command_list_append_image_copy_from_memory_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    size_t bytes_transferred = 0;
    const ze_image_region_t* region = *(params->ppDstRegion);

    if (region != nullptr) {
      bytes_transferred = region->width * region->height;
      if (region->depth != 0) {
        bytes_transferred *= region->depth;
      }
    }

    OnEnterKernelAppend(
        GetTransferProps(
            "zeCommandListAppendImageCopyFromMemory", bytes_transferred),
        *(params->phSignalEvent),
        *(params->phCommandList),
        global_data,
        instance_data);
  }

  static void OnExitCommandListAppendLaunchKernel(
      ze_command_list_append_launch_kernel_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendLaunchCooperativeKernel(
      ze_command_list_append_launch_cooperative_kernel_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendLaunchKernelIndirect(
      ze_command_list_append_launch_kernel_indirect_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendMemoryCopy(
      ze_command_list_append_memory_copy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendMemoryFill(
      ze_command_list_append_memory_fill_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendBarrier(
      ze_command_list_append_barrier_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendMemoryRangesBarrier(
      ze_command_list_append_memory_ranges_barrier_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendMemoryCopyRegion(
      ze_command_list_append_memory_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendMemoryCopyFromContext(
      ze_command_list_append_memory_copy_from_context_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendImageCopy(
      ze_command_list_append_image_copy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendImageCopyRegion(
      ze_command_list_append_image_copy_region_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendImageCopyToMemory(
      ze_command_list_append_image_copy_to_memory_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListAppendImageCopyFromMemory(
      ze_command_list_append_image_copy_from_memory_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    PTI_ASSERT(*(params->phSignalEvent) != nullptr);
    OnExitKernelAppend(*params->phCommandList, global_data,
                       instance_data, result);
  }

  static void OnExitCommandListCreate(
      ze_command_list_create_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(**params->pphCommandList != nullptr);
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->AddCommandList(
          **(params->pphCommandList),
          *(params->phContext),
          *(params->phDevice),
          false);
    }
  }

  static void OnExitCommandListCreateImmediate(
      ze_command_list_create_immediate_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(**params->pphCommandList != nullptr);
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->AddCommandList(
          **(params->pphCommandList),
          *(params->phContext),
          *(params->phDevice),
          true);
    }
  }

  static void OnExitCommandListDestroy(
      ze_command_list_destroy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*params->phCommandList != nullptr);
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->ProcessCalls();
      collector->RemoveCommandList(*params->phCommandList);
    }
  }

  static void OnExitCommandListReset(
      ze_command_list_reset_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      PTI_ASSERT(*params->phCommandList != nullptr);
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->ProcessCalls();
      collector->ResetCommandList(*params->phCommandList);
    }
  }

  static void OnEnterCommandQueueExecuteCommandLists(
      ze_command_queue_execute_command_lists_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    ZeKernelCollector* collector =
      reinterpret_cast<ZeKernelCollector*>(global_data);
    PTI_ASSERT(collector != nullptr);

    uint32_t command_list_count = *params->pnumCommandLists;
    if (command_list_count == 0) {
      return;
    }

    ze_command_list_handle_t* command_lists = *params->pphCommandLists;
    if (command_lists == nullptr) {
      return;
    }

    std::vector<ZeSubmitData>* submit_data_list =
      new std::vector<ZeSubmitData>();
    PTI_ASSERT(submit_data_list != nullptr);

    for (uint32_t i = 0; i < command_list_count; ++i) {
      ze_device_handle_t device =
        collector->GetCommandListDevice(command_lists[i]);
      PTI_ASSERT(device != nullptr);

      submit_data_list->push_back({
          collector->GetHostTimestamp(),
          collector->GetDeviceTimestamp(device)});
    }

    *reinterpret_cast<std::vector<ZeSubmitData>**>(instance_data) =
      submit_data_list;
  }

  static void OnExitCommandQueueExecuteCommandLists(
      ze_command_queue_execute_command_lists_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    std::vector<ZeSubmitData>* submit_data_list =
      *reinterpret_cast<std::vector<ZeSubmitData>**>(instance_data);
    PTI_ASSERT(submit_data_list != nullptr);

    if (result == ZE_RESULT_SUCCESS) {
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);

      uint32_t command_list_count = *params->pnumCommandLists;
      ze_command_list_handle_t* command_lists = *params->pphCommandLists;
      for (uint32_t i = 0; i < command_list_count; ++i) {
        if (!collector->IsCommandListImmediate(command_lists[i])) {
          collector->AddKernelCalls(
              command_lists[i],
              *(params->phCommandQueue),
              &submit_data_list->at(i));
        }
      }
    }

    delete submit_data_list;
  }

  static void OnExitCommandQueueSynchronize(
      ze_command_queue_synchronize_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->ProcessCalls();
    }
  }

  static void OnExitCommandQueueDestroy(
      ze_command_queue_destroy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->ProcessCalls();
    }
  }

  static void OnExitKernelSetGroupSize(
      ze_kernel_set_group_size_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      ZeKernelGroupSize group_size{
          *(params->pgroupSizeX),
          *(params->pgroupSizeY),
          *(params->pgroupSizeZ)};
      collector->AddKernelGroupSize(*(params->phKernel), group_size);
    }
  }

  static void OnExitKernelDestroy(
      ze_kernel_destroy_params_t* params,
      ze_result_t result, void* global_data, void** instance_data) {
    if (result == ZE_RESULT_SUCCESS) {
      ZeKernelCollector* collector =
        reinterpret_cast<ZeKernelCollector*>(global_data);
      PTI_ASSERT(collector != nullptr);
      collector->RemoveKernelGroupSize(*(params->phKernel));
    }
  }

 private: // Data
  zel_tracer_handle_t tracer_ = nullptr;

  bool verbose_ = false;

  Correlator* correlator_ = nullptr;
  std::atomic<uint64_t> kernel_id_;

  OnZeKernelFinishCallback callback_ = nullptr;
  void* callback_data_ = nullptr;

  std::mutex lock_;
  ZeKernelInfoMap kernel_info_map_;
  std::list<ZeKernelCall*> kernel_call_list_;
  ZeCommandListMap command_list_map_;
  ZeImageSizeMap image_size_map_;
  ZeKernelGroupSizeMap kernel_group_size_map_;

#ifdef PTI_KERNEL_INTERVALS
  ZeKernelIntervalList kernel_interval_list_;
  ZeDeviceMap device_map_;
#endif // PTI_KERNEL_INTERVALS

  static const uint32_t kKernelLength = 10;
  static const uint32_t kCallsLength = 12;
  static const uint32_t kTimeLength = 20;
  static const uint32_t kPercentLength = 10;
};

#endif // PTI_TOOLS_ZE_TRACER_ZE_KERNEL_COLLECTOR_H_