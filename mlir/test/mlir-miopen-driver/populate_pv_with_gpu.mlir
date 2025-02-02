// RUN: miopen-gen  -p -x2 -pv_with_gpu | FileCheck %s

// CHECK: func.func @miopen_conv2d_gkcyx_ngchw_ngkhw_0({{.*}}) attributes {kernel = 0 : i32} {
// CHECK: miopen.conv2d({{.*}}) {[[PARMS:.*]], xdlopsV2 = true} : memref<[[FILTERDIMS:[x0-9]+]]xf32>, memref<[[INPUTDIMS:[x0-9]+]]xf32>, memref<[[OUTPUTDIMS:[x0-9]+]]xf32>
// CHECK: call @miopen_conv2d_gkcyx_ngchw_ngkhw_0_gpu({{.*}}) : (memref<[[FILTERDIMS]]xf32>, memref<[[INPUTDIMS]]xf32>, memref<[[OUTPUTDIMS]]xf32>) -> ()
// CHECK: call @miopen_conv2d_gkcyx_ngchw_ngkhw_0_ver_gpu({{.*}}) : (memref<[[FILTERDIMS]]xf32>, memref<[[INPUTDIMS]]xf32>, memref<[[OUTPUTDIMS]]xf32>) -> ()
// CHECK: func.func @miopen_conv2d_gkcyx_ngchw_ngkhw_0_ver({{.*}}) attributes {kernel = 0 : i32} {
// CHECK: miopen.conv2d({{.*}}) {{{.*}} : memref<[[FILTERDIMS]]xf32>, memref<[[INPUTDIMS]]xf32>, memref<[[OUTPUTDIMS]]xf32>

// RUN: miopen-gen  -p -t f16 -pv_with_gpu | FileCheck %s --check-prefix=F16-CHECK

// F16-CHECK: func.func @miopen_conv2d_gkcyx_ngchw_ngkhw_0({{.*}}) attributes {kernel = 0 : i32} {
// F16-CHECK: miopen.conv2d({{.*}}) {[[PARMS:.*]]} : memref<[[FILTERDIMS:[x0-9]+]]xf16>, memref<[[INPUTDIMS:[x0-9]+]]xf16>, memref<[[OUTPUTDIMS:[x0-9]+]]xf16>
// F16-CHECK: call @miopen_conv2d_gkcyx_ngchw_ngkhw_0_gpu({{.*}}) : (memref<[[FILTERDIMS]]xf16>, memref<[[INPUTDIMS]]xf16>, memref<[[OUTPUTDIMS]]xf16>) -> ()
// F16-CHECK: call @miopen_conv2d_gkcyx_ngchw_ngkhw_0_ver_gpu({{.*}}) : (memref<[[FILTERDIMS]]xf32>, memref<[[INPUTDIMS]]xf32>, memref<[[OUTPUTDIMS]]xf32>) -> ()
// F16-CHECK: func.func @miopen_conv2d_gkcyx_ngchw_ngkhw_0_ver({{.*}}) attributes {kernel = 0 : i32} {
// F16-CHECK: miopen.conv2d({{.*}}) {{{.*}} : memref<[[FILTERDIMS]]xf32>, memref<[[INPUTDIMS]]xf32>, memref<[[OUTPUTDIMS]]xf32>
