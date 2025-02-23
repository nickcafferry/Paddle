// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/fluid/framework/data_layout_transform.h"
#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/scope.h"
#include "paddle/fluid/inference/api/paddle_inference_api.h"
#include "paddle/fluid/inference/api/paddle_tensor.h"
#include "paddle/fluid/memory/memcpy.h"
#include "paddle/fluid/platform/enforce.h"
#include "paddle/fluid/platform/float16.h"

namespace paddle_infer {

using float16 = paddle::platform::float16;

void Tensor::Reshape(const std::vector<int> &shape) {
  PADDLE_ENFORCE_EQ(
      name_.empty(), false,
      paddle::platform::errors::PreconditionNotMet(
          "Need to SetName first, so that the corresponding tensor can "
          "be retrieved."));
  PADDLE_ENFORCE_EQ(input_or_output_, true,
                    paddle::platform::errors::PermissionDenied(
                        "Can't reshape the output tensor, it is readonly"));
  auto *scope = static_cast<paddle::framework::Scope *>(scope_);
  auto *var = scope->FindVar(name_);
  PADDLE_ENFORCE_NOT_NULL(
      var, paddle::platform::errors::PreconditionNotMet(
               "No tensor called [%s] in the runtime scope", name_));
  auto *tensor = var->GetMutable<paddle::framework::LoDTensor>();
  tensor->Resize(paddle::framework::make_ddim(shape));
}

void Tensor::ReshapeStrings(const size_t &shape) {
  PADDLE_ENFORCE_EQ(
      name_.empty(), false,
      paddle::platform::errors::PreconditionNotMet(
          "Need to SetName first, so that the corresponding tensor can "
          "be retrieved."));
  PADDLE_ENFORCE_EQ(input_or_output_, true,
                    paddle::platform::errors::PermissionDenied(
                        "Can't reshape the output tensor, it is readonly"));
  auto *scope = static_cast<paddle::framework::Scope *>(scope_);
  auto *var = scope->FindVar(name_);
  PADDLE_ENFORCE_NOT_NULL(
      var, paddle::platform::errors::PreconditionNotMet(
               "No tensor called [%s] in the runtime scope", name_));
  paddle_infer::Strings *tensor = var->GetMutable<paddle_infer::Strings>();
  tensor->resize(shape);
}

#define EAGER_GET_TENSOR(tensor_type)    \
  if (!tensor_) {                        \
    tensor_ = FindTensor<tensor_type>(); \
  }                                      \
  auto *tensor = static_cast<tensor_type *>(tensor_);

template <typename T>
T *Tensor::mutable_data(PlaceType place) {
  EAGER_GET_TENSOR(paddle::framework::LoDTensor);
  PADDLE_ENFORCE_GT(
      tensor->numel(), 0,
      paddle::platform::errors::PreconditionNotMet(
          "You should call Tensor::Reshape(const std::vector<int> "
          "&shape)"
          "function before retrieving mutable_data from input tensor."));
  switch (static_cast<int>(place)) {
    case static_cast<int>(PlaceType::kCPU): {
      return tensor->mutable_data<T>(paddle::platform::CPUPlace());
    }
    case static_cast<int>(PlaceType::kGPU): {
      return tensor->mutable_data<T>(paddle::platform::CUDAPlace(device_));
    }
    case static_cast<int>(PlaceType::kXPU): {
      return tensor->mutable_data<T>(paddle::platform::XPUPlace(device_));
    }
    case static_cast<int>(PlaceType::kNPU): {
      return tensor->mutable_data<T>(paddle::platform::NPUPlace(device_));
    }
    default:
      PADDLE_THROW(paddle::platform::errors::Unavailable(
          "Only CPU / CUDA / XPU / NPU places is supported. The place `%d` is "
          "not supported.",
          static_cast<int>(place)));
      break;
  }
  return nullptr;
}

template <typename T>
T *Tensor::data(PlaceType *place, int *size) const {
  EAGER_GET_TENSOR(paddle::framework::LoDTensor);
  auto *res = tensor->data<T>();

  if (paddle::platform::is_cpu_place(tensor->place())) {
    *place = PlaceType::kCPU;
  } else if (paddle::platform::is_gpu_place(tensor->place())) {
    *place = PlaceType::kGPU;
  } else if (paddle::platform::is_xpu_place(tensor->place())) {
    *place = PlaceType::kXPU;
  } else if (paddle::platform::is_npu_place(tensor->place())) {
    *place = PlaceType::kNPU;
  } else {
    *place = PlaceType::kUNK;
  }

  *size = tensor->numel();
  return res;
}

DataType Tensor::type() const {
  EAGER_GET_TENSOR(paddle::framework::LoDTensor);
  auto type = tensor->type();
  if (type == paddle::framework::proto::VarType::FP32) {
    return DataType::FLOAT32;
  } else if (type == paddle::framework::proto::VarType::FP16) {
    return DataType::FLOAT16;
  } else if (type == paddle::framework::proto::VarType::INT64) {
    return DataType::INT64;
  } else if (type == paddle::framework::proto::VarType::INT32) {
    return DataType::INT32;
  } else if (type == paddle::framework::proto::VarType::UINT8) {
    return DataType::UINT8;
  } else if (type == paddle::framework::proto::VarType::INT8) {
    return DataType::INT8;
  }
  return DataType::FLOAT32;
}

PlaceType Tensor::place() const { return place_; }

template <typename T>
void Tensor::CopyFromCpu(const T *data) {
  EAGER_GET_TENSOR(paddle::framework::LoDTensor);
  PADDLE_ENFORCE_GE(tensor->numel(), 0,
                    paddle::platform::errors::PreconditionNotMet(
                        "You should call Tensor::Reshape(const "
                        "std::vector<int> &shape)"
                        "function before copying data from cpu."));
  size_t ele_size = tensor->numel() * sizeof(T);

  if (place_ == PlaceType::kCPU) {
    auto *t_data = tensor->mutable_data<T>(paddle::platform::CPUPlace());
    std::memcpy(static_cast<void *>(t_data), data, ele_size);
  } else if (place_ == PlaceType::kGPU) {
#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
    paddle::platform::DeviceContextPool &pool =
        paddle::platform::DeviceContextPool::Instance();
    paddle::platform::CUDAPlace gpu_place(device_);
    auto *t_data = tensor->mutable_data<T>(gpu_place);
    auto *dev_ctx = static_cast<const paddle::platform::CUDADeviceContext *>(
        pool.Get(gpu_place));

    paddle::memory::Copy(gpu_place, static_cast<void *>(t_data),
                         paddle::platform::CPUPlace(), data, ele_size,
                         dev_ctx->stream());
#else
    PADDLE_THROW(paddle::platform::errors::Unavailable(
        "Can not create tensor with CUDA place because paddle is not compiled "
        "with CUDA."));
#endif
  } else if (place_ == PlaceType::kXPU) {
#ifdef PADDLE_WITH_XPU
    paddle::platform::XPUPlace xpu_place(device_);
    auto *t_data = tensor->mutable_data<T>(xpu_place);
    paddle::memory::Copy(xpu_place, static_cast<void *>(t_data),
                         paddle::platform::CPUPlace(), data, ele_size);
#else
    PADDLE_THROW(paddle::platform::errors::Unavailable(
        "Can not create tensor with XPU place because paddle is not compiled "
        "with XPU."));
#endif
  } else if (place_ == PlaceType::kNPU) {
#ifdef PADDLE_WITH_ASCEND_CL
    paddle::platform::DeviceContextPool &pool =
        paddle::platform::DeviceContextPool::Instance();
    paddle::platform::NPUPlace npu_place(device_);
    auto *t_data = tensor->mutable_data<T>(npu_place);
    auto *dev_ctx = static_cast<const paddle::platform::NPUDeviceContext *>(
        pool.Get(npu_place));
    paddle::memory::Copy(npu_place, static_cast<void *>(t_data),
                         paddle::platform::CPUPlace(), data, ele_size,
                         dev_ctx->stream());
#else
    PADDLE_THROW(paddle::platform::errors::Unavailable(
        "Can not create tensor with NPU place because paddle is not compiled "
        "with NPU."));
#endif
  } else {
    PADDLE_THROW(paddle::platform::errors::InvalidArgument(
        "The analysis predictor supports CPU, GPU, NPU and XPU now."));
  }
}

void Tensor::CopyStringsFromCpu(const paddle_infer::Strings *data) {
  EAGER_GET_TENSOR(paddle_infer::Strings);
  PADDLE_ENFORCE_GE(tensor->size(), 0,
                    paddle::platform::errors::PreconditionNotMet(
                        "You should call Tensor::Reshape(const "
                        "std::size_t &shape)function before copying"
                        "the string data from cpu."));
  *tensor = *data;
}

template <typename T>
void Tensor::CopyToCpuImpl(T *data, void *exec_stream, CallbackFunc cb,
                           void *cb_params) const {
  EAGER_GET_TENSOR(paddle::framework::LoDTensor);
  auto ele_num = tensor->numel();
  auto *t_data = tensor->data<T>();
  auto t_place = tensor->place();

  paddle::framework::Tensor out;
  auto mem_allocation = std::make_shared<paddle::memory::Allocation>(
      static_cast<void *>(data), ele_num * sizeof(T),
      paddle::platform::CPUPlace());
  out.ResetHolder(mem_allocation);

  if (paddle::platform::is_cpu_place(t_place)) {
#ifdef PADDLE_WITH_MKLDNN
    if (tensor->layout() == paddle::framework::DataLayout::kMKLDNN)
      paddle::framework::innerTransDataLayoutFromMKLDNN(
          tensor->layout(), paddle::platform::MKLDNNDeviceContext::tls()
                                .get_cur_paddle_data_layout(),
          *tensor, &out, paddle::platform::CPUPlace(), true);
    else
      std::memcpy(static_cast<void *>(data), t_data, ele_num * sizeof(T));
#else
    std::memcpy(static_cast<void *>(data), t_data, ele_num * sizeof(T));
#endif
  } else if (place_ == PlaceType::kGPU) {
#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
    paddle::platform::DeviceContextPool &pool =
        paddle::platform::DeviceContextPool::Instance();
    auto gpu_place = BOOST_GET_CONST(paddle::platform::CUDAPlace, t_place);
    auto *dev_ctx = static_cast<const paddle::platform::CUDADeviceContext *>(
        pool.Get(gpu_place));
    paddle::memory::Copy(paddle::platform::CPUPlace(),
                         static_cast<void *>(data), gpu_place, t_data,
                         ele_num * sizeof(T), dev_ctx->stream());
#ifdef PADDLE_WITH_HIP
    hipStreamSynchronize(dev_ctx->stream());
#else
    // async, return stream
    if (nullptr != exec_stream) {
      *(static_cast<cudaStream_t *>(exec_stream)) = dev_ctx->stream();
      // async with callback
    } else if (cb) {
      cudaLaunchHostFunc(dev_ctx->stream(), cb, cb_params);
      // sync
    } else {
      cudaStreamSynchronize(dev_ctx->stream());
    }
#endif
#else
    PADDLE_THROW(paddle::platform::errors::Unavailable(
        "Can not create tensor with CUDA place because paddle is not compiled "
        "with CUDA."));
#endif
  } else if (place_ == PlaceType::kXPU) {
#ifdef PADDLE_WITH_XPU
    auto xpu_place = BOOST_GET_CONST(paddle::platform::XPUPlace, t_place);
    paddle::memory::Copy(paddle::platform::CPUPlace(),
                         static_cast<void *>(data), xpu_place, t_data,
                         ele_num * sizeof(T));
#else
    PADDLE_THROW(paddle::platform::errors::Unavailable(
        "Can not create tensor with XPU place because paddle is not compiled "
        "with XPU."));
#endif
  } else if (place_ == PlaceType::kNPU) {
#ifdef PADDLE_WITH_ASCEND_CL
    paddle::platform::DeviceContextPool &pool =
        paddle::platform::DeviceContextPool::Instance();
    auto npu_place = BOOST_GET_CONST(paddle::platform::NPUPlace, t_place);
    auto *dev_ctx = static_cast<const paddle::platform::NPUDeviceContext *>(
        pool.Get(npu_place));
    paddle::memory::Copy(paddle::platform::CPUPlace(),
                         static_cast<void *>(data), npu_place, t_data,
                         ele_num * sizeof(T), dev_ctx->stream());
    paddle::platform::NPUStreamSync(dev_ctx->stream());
#else
    PADDLE_THROW(paddle::platform::errors::Unavailable(
        "Can not create tensor with NPU place because paddle is not compiled "
        "with NPU."));
#endif
  } else {
    PADDLE_THROW(paddle::platform::errors::InvalidArgument(
        "The analysis predictor supports CPU, GPU, NPU and XPU now."));
  }
}

template <typename T>
void Tensor::CopyToCpu(T *data) const {
  CopyToCpuImpl<T>(data, nullptr, nullptr, nullptr);
}

template <typename T>
void Tensor::CopyToCpuAsync(T *data, void *exec_stream) const {
  CopyToCpuImpl<T>(data, exec_stream, nullptr, nullptr);
}

template <typename T>
void Tensor::CopyToCpuAsync(T *data, CallbackFunc cb, void *cb_params) const {
  CopyToCpuImpl<T>(data, nullptr, cb, cb_params);
}

template PD_INFER_DECL void Tensor::CopyFromCpu<float>(const float *data);
template PD_INFER_DECL void Tensor::CopyFromCpu<int64_t>(const int64_t *data);
template PD_INFER_DECL void Tensor::CopyFromCpu<int32_t>(const int32_t *data);
template PD_INFER_DECL void Tensor::CopyFromCpu<uint8_t>(const uint8_t *data);
template PD_INFER_DECL void Tensor::CopyFromCpu<int8_t>(const int8_t *data);
template PD_INFER_DECL void Tensor::CopyFromCpu<float16>(const float16 *data);

template PD_INFER_DECL void Tensor::CopyToCpu<float>(float *data) const;
template PD_INFER_DECL void Tensor::CopyToCpu<int64_t>(int64_t *data) const;
template PD_INFER_DECL void Tensor::CopyToCpu<int32_t>(int32_t *data) const;
template PD_INFER_DECL void Tensor::CopyToCpu<uint8_t>(uint8_t *data) const;
template PD_INFER_DECL void Tensor::CopyToCpu<int8_t>(int8_t *data) const;
template PD_INFER_DECL void Tensor::CopyToCpu<float16>(float16 *data) const;

template PD_INFER_DECL void Tensor::CopyToCpuImpl<float>(float *data,
                                                         void *exec_stream,
                                                         CallbackFunc cb,
                                                         void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuImpl<int64_t>(
    int64_t *data, void *exec_stream, CallbackFunc cb, void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuImpl<int32_t>(
    int32_t *data, void *exec_stream, CallbackFunc cb, void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuImpl<uint8_t>(
    uint8_t *data, void *exec_stream, CallbackFunc cb, void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuImpl<int8_t>(
    int8_t *data, void *exec_stream, CallbackFunc cb, void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuImpl<float16>(
    float16 *data, void *exec_stream, CallbackFunc cb, void *cb_params) const;

template PD_INFER_DECL void Tensor::CopyToCpuAsync<float>(
    float *data, void *exec_stream) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<int64_t>(
    int64_t *data, void *exec_stream) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<int32_t>(
    int32_t *data, void *exec_stream) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<uint8_t>(
    uint8_t *data, void *exec_stream) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<int8_t>(
    int8_t *data, void *exec_stream) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<float16>(
    float16 *data, void *exec_stream) const;

template PD_INFER_DECL void Tensor::CopyToCpuAsync<float>(
    float *data, CallbackFunc cb, void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<int64_t>(
    int64_t *data, CallbackFunc cb, void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<int32_t>(
    int32_t *data, CallbackFunc cb, void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<uint8_t>(
    uint8_t *data, CallbackFunc cb, void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<int8_t>(
    int8_t *data, CallbackFunc cb, void *cb_params) const;
template PD_INFER_DECL void Tensor::CopyToCpuAsync<float16>(
    float16 *data, CallbackFunc cb, void *cb_params) const;

template PD_INFER_DECL float *Tensor::data<float>(PlaceType *place,
                                                  int *size) const;
template PD_INFER_DECL int64_t *Tensor::data<int64_t>(PlaceType *place,
                                                      int *size) const;
template PD_INFER_DECL int32_t *Tensor::data<int32_t>(PlaceType *place,
                                                      int *size) const;
template PD_INFER_DECL uint8_t *Tensor::data<uint8_t>(PlaceType *place,
                                                      int *size) const;
template PD_INFER_DECL int8_t *Tensor::data<int8_t>(PlaceType *place,
                                                    int *size) const;
template PD_INFER_DECL float16 *Tensor::data<float16>(PlaceType *place,
                                                      int *size) const;

template PD_INFER_DECL float *Tensor::mutable_data<float>(PlaceType place);
template PD_INFER_DECL int64_t *Tensor::mutable_data<int64_t>(PlaceType place);
template PD_INFER_DECL int32_t *Tensor::mutable_data<int32_t>(PlaceType place);
template PD_INFER_DECL uint8_t *Tensor::mutable_data<uint8_t>(PlaceType place);
template PD_INFER_DECL int8_t *Tensor::mutable_data<int8_t>(PlaceType place);
template PD_INFER_DECL float16 *Tensor::mutable_data<float16>(PlaceType place);

Tensor::Tensor(void *scope) : scope_{scope} {
  PADDLE_ENFORCE_NOT_NULL(scope_,
                          paddle::platform::errors::PreconditionNotMet(
                              "The `scope` can not be nullptr. It should be "
                              "set to the pointer of scope."));
}

template <typename T>
void *Tensor::FindTensor() const {
  PADDLE_ENFORCE_EQ(
      name_.empty(), false,
      paddle::platform::errors::PreconditionNotMet(
          "Need to SetName first, so that the corresponding tensor can "
          "be retrieved."));
  auto *scope = static_cast<paddle::framework::Scope *>(scope_);
  auto *var = scope->FindVar(name_);
  PADDLE_ENFORCE_NOT_NULL(
      var, paddle::platform::errors::PreconditionNotMet(
               "No tensor called [%s] in the runtime scope", name_));
  auto *tensor = var->GetMutable<T>();
  return tensor;
}

std::vector<int> Tensor::shape() const {
  EAGER_GET_TENSOR(paddle::framework::LoDTensor);
  PADDLE_ENFORCE_NOT_NULL(
      tensor_, paddle::platform::errors::PreconditionNotMet(
                   "Not found tensor called %s in the scope", name_));
  return paddle::framework::vectorize<int>(tensor->dims());
}

void Tensor::SetLoD(const std::vector<std::vector<size_t>> &x) {
  EAGER_GET_TENSOR(paddle::framework::LoDTensor);
  paddle::framework::LoD lod;
  for (auto &level : x) {
    lod.emplace_back(level);
  }
  tensor->set_lod(lod);
}

std::vector<std::vector<size_t>> Tensor::lod() const {
  EAGER_GET_TENSOR(paddle::framework::LoDTensor);
  std::vector<std::vector<size_t>> res;
  for (auto &level : tensor->lod()) {
    res.emplace_back(level);
  }
  return res;
}

void Tensor::SetName(const std::string &name) { name_ = name; }

const std::string &Tensor::name() const { return name_; }

void Tensor::SetPlace(PlaceType place, int device) {
  place_ = place;
  device_ = device;
}

}  // namespace paddle_infer
