if(NOT WITH_XPU)
  return()
endif()

set(XPU_CTX_DEPS xpulib ssl crypto rt z resolv dl)

cc_library(xpu_info SRCS xpu_info.cc DEPS gflags glog enforce xpulib)
cc_library(xpu_op_list SRCS xpu_op_list.cc DEPS gflags glog enforce xpulib device_context)
