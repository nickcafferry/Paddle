if(WITH_GPU)
  nv_library(pten_transpose_cuda SRCS transpose.cu DEPS dense_tensor malloc device_context)
elseif(WITH_ROCM)
  hip_library(pten_transpose_cuda SRCS transpose.cu DEPS dense_tensor malloc device_context)
endif()
