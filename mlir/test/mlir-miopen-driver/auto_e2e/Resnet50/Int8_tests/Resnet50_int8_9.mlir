//This file is automatically generated. Do not edit!

// RUN: miopen-gen -batchsize=256 -in_channels=256 -in_h=14 -in_w=14 -out_channels=256 -fil_h=3 -fil_w=3 --dilation_h=1 --dilation_w=1 --conv_stride_h=1 --conv_stride_w=1 --padding_h=1 --padding_w=1 --operation conv2d -fil_layout=gkcyx -in_layout=ngchw -out_layout=ngkhw -t i8  %pv %random_data %xdlops | mlir-miopen-driver -c | mlir-rocm-runner --shared-libs=%linalg_test_lib_dir/libmlir_rocm_runtime%shlibext,%conv_validation_wrapper_library_dir/libconv-validation-wrappers%shlibext,%linalg_test_lib_dir/libmlir_runner_utils%shlibext --entry-point-result=void | FileCheck %s --check-prefix=CHECK_Resnet50_int8_9_1
// CHECK_Resnet50_int8_9_1: Unranked Memref base@ = 0x{{.*}} rank = 1 offset = 0 sizes = [1] strides = [1] data =
// CHECK_Resnet50_int8_9_1: [1]

// RUN: miopen-gen -batchsize=256 -in_channels=256 -in_h=14 -in_w=14 -out_channels=256 -fil_h=3 -fil_w=3 --dilation_h=1 --dilation_w=1 --conv_stride_h=1 --conv_stride_w=1 --padding_h=1 --padding_w=1 --operation conv2d -fil_layout=gkyxc -in_layout=nhwgc -out_layout=nhwgk -t i8  %pv %random_data %xdlops | mlir-miopen-driver -c | mlir-rocm-runner --shared-libs=%linalg_test_lib_dir/libmlir_rocm_runtime%shlibext,%conv_validation_wrapper_library_dir/libconv-validation-wrappers%shlibext,%linalg_test_lib_dir/libmlir_runner_utils%shlibext --entry-point-result=void | FileCheck %s --check-prefix=CHECK_Resnet50_int8_9_2
// CHECK_Resnet50_int8_9_2: Unranked Memref base@ = 0x{{.*}} rank = 1 offset = 0 sizes = [1] strides = [1] data =
// CHECK_Resnet50_int8_9_2: [1]

