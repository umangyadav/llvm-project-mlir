// RUN: mlir-miopen-driver -host-pipeline highlevel %s | miopen-gen -ph -print-inputs -print-results -rand none - | mlir-miopen-driver -c  | mlir-rocm-runner --shared-libs=%linalg_test_lib_dir/libmlir_rocm_runtime%shlibext,%conv_validation_wrapper_library_dir/libconv-validation-wrappers%shlibext,%linalg_test_lib_dir/libmlir_runner_utils%shlibext -entry-point-result=void | FileCheck %s

// CHECK: Unranked Memref base
//func.func @test_fusion(%arg0: tensor<1x8x8x4xf32>, %arg1: tensor<8x1x1x4xf32>, %arg3: tensor<1x1x1x8xf32>) -> tensor<1x8x8x8xf32> attributes {kernel} {
func.func @test_fusion(%arg0: tensor<1x8x8x4xf32>, %arg1: tensor<8x1x1x4xf32>) -> tensor<1x8x8x8xf32> attributes {kernel} {
  %zero = arith.constant dense<0.0> : tensor<8xf32>
  %0 = "tosa.conv2d"(%arg0, %arg1, %zero) {dilation = [1, 1], pad = [0, 0, 0, 0], stride = [1, 1]} : (tensor<1x8x8x4xf32>, tensor<8x1x1x4xf32>, tensor<8xf32>) -> tensor<1x8x8x8xf32>
//  %2 = "tosa.add"(%0, %arg3) {} : (tensor<1x8x8x8xf32>, tensor<1x1x1x8xf32>) -> tensor<1x8x8x8xf32>

  //return %2 : tensor<1x8x8x8xf32>
  return %0 : tensor<1x8x8x8xf32>
}

// -----

