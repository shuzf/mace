// Copyright 2018 Xiaomi, Inc.  All rights reserved.
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

#include "mace/ops/ops_test_util.h"
#include "mace/core/memory_optimizer.h"

namespace mace {
namespace ops {
namespace test {


OpDefBuilder::OpDefBuilder(const char *type, const std::string &name) {
  op_def_.set_type(type);
  op_def_.set_name(name);
}

OpDefBuilder &OpDefBuilder::Input(const std::string &input_name) {
  op_def_.add_input(input_name);
  return *this;
}

OpDefBuilder &OpDefBuilder::Output(const std::string &output_name) {
  op_def_.add_output(output_name);
  return *this;
}

OpDefBuilder &OpDefBuilder::OutputType(
    const std::vector<DataType> &output_type) {
  for (auto out_t : output_type) {
    op_def_.add_output_type(out_t);
  }
  return *this;
}

OpDefBuilder &OpDefBuilder::OutputShape(
    const std::vector<mace::index_t> &output_shape) {
  auto shape = op_def_.add_output_shape();
  for (auto s : output_shape) {
    shape->add_dims(s);
  }
  return *this;
}

OpDefBuilder OpDefBuilder::AddIntArg(const std::string &name, const int value) {
  auto arg = op_def_.add_arg();
  arg->set_name(name);
  arg->set_i(value);
  return *this;
}

OpDefBuilder OpDefBuilder::AddFloatArg(const std::string &name,
                                       const float value) {
  auto arg = op_def_.add_arg();
  arg->set_name(name);
  arg->set_f(value);
  return *this;
}

OpDefBuilder OpDefBuilder::AddStringArg(const std::string &name,
                                        const char *value) {
  auto arg = op_def_.add_arg();
  arg->set_name(name);
  arg->set_s(value);
  return *this;
}

OpDefBuilder OpDefBuilder::AddIntsArg(const std::string &name,
                                      const std::vector<int> &values) {
  auto arg = op_def_.add_arg();
  arg->set_name(name);
  for (auto value : values) {
    arg->add_ints(value);
  }
  return *this;
}

OpDefBuilder OpDefBuilder::AddFloatsArg(const std::string &name,
                                        const std::vector<float> &values) {
  auto arg = op_def_.add_arg();
  arg->set_name(name);
  for (auto value : values) {
    arg->add_floats(value);
  }
  return *this;
}

void OpDefBuilder::Finalize(OperatorDef *op_def) const {
  MACE_CHECK(op_def != nullptr, "input should not be null.");
  *op_def = op_def_;
}

namespace {
std::string GetStoragePathFromEnv() {
  char *storage_path_str = getenv("MACE_INTERNAL_STORAGE_PATH");
  if (storage_path_str == nullptr) return "";
  return storage_path_str;
}
}  // namespace

OpTestContext *OpTestContext::Get(int num_threads,
                                  CPUAffinityPolicy cpu_affinity_policy,
                                  bool use_gemmlowp) {
  static OpTestContext instance(num_threads,
                                cpu_affinity_policy,
                                use_gemmlowp);
  return &instance;
}

OpTestContext::OpTestContext(int num_threads,
                             CPUAffinityPolicy cpu_affinity_policy,
                             bool use_gemmlowp)
    : gpu_context_(new GPUContext(GetStoragePathFromEnv())),
      opencl_mem_types_({MemoryType::GPU_IMAGE}) {
  device_map_[DeviceType::CPU] = std::unique_ptr<Device>(
      new CPUDevice(num_threads,
                    cpu_affinity_policy,
                    use_gemmlowp));

  device_map_[DeviceType::GPU] = std::unique_ptr<Device>(
      new GPUDevice(gpu_context_->opencl_tuner(),
                    gpu_context_->opencl_cache_storage(),
                    GPUPriorityHint::PRIORITY_NORMAL));
}

std::shared_ptr<GPUContext> OpTestContext::gpu_context() const {
  return gpu_context_;
}

Device *OpTestContext::GetDevice(DeviceType device_type) {
  return device_map_[device_type].get();
}

std::vector<MemoryType> OpTestContext::opencl_mem_types() {
  return opencl_mem_types_;
}

void OpTestContext::SetOCLBufferTestFlag() {
  opencl_mem_types_ = {MemoryType::GPU_BUFFER};
}

void OpTestContext::SetOCLImageTestFlag() {
  opencl_mem_types_ = {MemoryType::GPU_IMAGE};
}

void OpTestContext::SetOCLImageAndBufferTestFlag() {
  opencl_mem_types_ = {MemoryType::GPU_IMAGE, MemoryType::GPU_BUFFER};
}

bool OpsTestNet::Setup(mace::DeviceType device) {
  NetDef net_def;
  for (auto &op_def_ : op_defs_) {
    net_def.add_op()->CopyFrom(op_def_);

    for (auto input : op_def_.input()) {
      if (ws_.GetTensor(input) != nullptr &&
          !ws_.GetTensor(input)->is_weight()) {
        auto input_info = net_def.add_input_info();
        input_info->set_name(input);
        auto &shape = ws_.GetTensor(input)->shape();
        for (auto d : shape) {
          input_info->add_dims(static_cast<int>(d));
        }
      }
    }

    for (int i = 0; i < op_def_.output_size(); ++i) {
      ws_.RemoveTensor(op_def_.output(i));
      auto output_info = net_def.add_output_info();
      output_info->set_name(op_def_.output(i));
      if (op_def_.output_type_size() == op_def_.output_size()) {
        output_info->set_data_type(op_def_.output_type(i));
      } else {
        output_info->set_data_type(DataType::DT_FLOAT);
      }
    }
  }
  MemoryOptimizer mem_optimizer;
  net_ = std::unique_ptr<NetBase>(new SerialNet(
      op_registry_.get(),
      &net_def,
      &ws_,
      OpTestContext::Get()->GetDevice(device),
      &mem_optimizer));
  MaceStatus status = (ws_.PreallocateOutputTensor(
      net_def,
      &mem_optimizer,
      OpTestContext::Get()->GetDevice(device)));
  if (status != MaceStatus::MACE_SUCCESS) return false;
  status = net_->Init();
  device_type_ = device;
  return status == MaceStatus::MACE_SUCCESS;
}

MaceStatus OpsTestNet::Run() {
  MACE_CHECK_NOTNULL(net_);
  MACE_RETURN_IF_ERROR(net_->Run());
  Sync();
  return MaceStatus::MACE_SUCCESS;
}

MaceStatus OpsTestNet::RunOp(mace::DeviceType device) {
  if (device == DeviceType::GPU) {
    auto opencl_mem_types = OpTestContext::Get()->opencl_mem_types();
    for (auto type : opencl_mem_types) {
      OpTestContext::Get()->GetDevice(device)
          ->gpu_runtime()->set_mem_type(type);
      Setup(device);
      MACE_RETURN_IF_ERROR(Run());
    }
    return MaceStatus::MACE_SUCCESS;
  } else {
    Setup(device);
    return Run();
  }
}

MaceStatus OpsTestNet::RunOp() {
  return RunOp(DeviceType::CPU);
}

MaceStatus OpsTestNet::RunNet(const mace::NetDef &net_def,
                              const mace::DeviceType device) {
  device_type_ = device;
  MemoryOptimizer mem_optimizer;
  net_ = std::unique_ptr<NetBase>(new SerialNet(
      op_registry_.get(),
      &net_def,
      &ws_,
      OpTestContext::Get()->GetDevice(device),
      &mem_optimizer));
  MACE_RETURN_IF_ERROR(ws_.PreallocateOutputTensor(
      net_def,
      &mem_optimizer,
      OpTestContext::Get()->GetDevice(device)));
  MACE_RETURN_IF_ERROR(net_->Init());
  return net_->Run();
}

void OpsTestNet::Sync() {
#ifdef MACE_ENABLE_OPENCL
  if (net_ && device_type_ == DeviceType::GPU) {
      OpTestContext::Get()->GetDevice(DeviceType::GPU)->gpu_runtime()
      ->opencl_runtime()->command_queue().finish();
    }
#endif
}

}  // namespace test
}  // namespace ops
}  // namespace mace
