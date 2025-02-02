// RUN: miopen-gen -batchsize=1024 -in_channels=1024 -out_channels=1024 -fil_w=1 -fil_h=1 -in_h=14 -in_w=14 -padding_h=1 -padding_w=1 | mlir-miopen-driver -miopen-affix-params -miopen-conv-to-gemm -miopen-gridwise-gemm-to-blockwise | FileCheck %s
// CHECK: leftOobDims = [3 : i32, 4 : i32]
// CHECK-SAME: rightOobDims = [3 : i32, 4 : i32]
