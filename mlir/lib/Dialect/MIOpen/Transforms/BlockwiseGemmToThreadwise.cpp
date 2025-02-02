//===- BlockwiseGemmToThreadwise - MLIR MIOpen ops lowering passes ---===//
//
// Copyright 2020 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ============================================================
//
// This pass converts miopen.blockwise_* ops to miopen.threadwise_*
// and lowers other higher-level ops like transform and fill in preparation for
// the threadwise lowering
//
//===-----------------------------------------------------===//
#include "PassDetail.h"

#include "mlir/Dialect/MIOpen/MIOpen.h"
#include "mlir/Dialect/MIOpen/Passes.h"
#include "mlir/Dialect/MIOpen/TransformMapBuilder.h"
#include "mlir/Dialect/MIOpen/utility/builderUtils.h"
#include "mlir/Dialect/MIOpen/utility/loweringUtils.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/MIOpen/XdlopsCodeSelection.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Transforms/DialectConversion.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "miopen-blockwise-to-threadwise"

using namespace mlir;
using namespace mlir::arith;
using namespace mlir::miopen;

namespace {
struct MIOpenLowerBlockwiseGemmToThreadwisePass
    : public MIOpenBlockwiseGemmToThreadwisePassBase<
          MIOpenLowerBlockwiseGemmToThreadwisePass> {
  void runOnOperation() override;
};

//===----------------------------------------------------------------------===//
// Fill lowering.
//===----------------------------------------------------------------------===//

struct FillRewritePattern : public OpConversionPattern<FillOp> {
  using OpConversionPattern<FillOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(FillOp op, FillOpAdaptor adaptor,
                                ConversionPatternRewriter &b) const override {
    Location loc = op.getLoc();
    auto inputType = op.input().getType().cast<MemRefType>();
    ArrayRef<int64_t> inputShape = inputType.getShape();
    llvm::SmallVector<int64_t> lbs(inputShape.size(), 0);
    llvm::SmallVector<int64_t> strides(inputShape.size(), 1);

    buildAffineLoopNest(b, loc, lbs, inputShape, strides,
                        [value = adaptor.value(), input = adaptor.input()](
                            OpBuilder &b, Location loc, ValueRange ivs) {
                          b.create<memref::StoreOp>(loc, value, input, ivs);
                        });

    b.replaceOp(op, {});
    return success();
  }
};

//===----------------------------------------------------------------------===//
// BlockwiseGemm lowering.
//===----------------------------------------------------------------------===//

struct BlockwiseGemmRewritePattern
    : public OpConversionPattern<BlockwiseGemmOp> {
  using OpConversionPattern<BlockwiseGemmOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(BlockwiseGemmOp op,
                                BlockwiseGemmOpAdaptor adaptor,
                                ConversionPatternRewriter &b) const override {
    Location loc = op.getLoc();

    // Prepare some useful constants.
    Value zeroConstantOp = b.createOrFold<ConstantIndexOp>(loc, 0);

    auto blockAType = op.matrixA().getType().cast<MemRefType>();
    auto blockBType = op.matrixB().getType().cast<MemRefType>();
    auto bufferCType = op.matrixC().getType().cast<MemRefType>();

    auto elementType = bufferCType.getElementType();

    int64_t k = blockAType.getShape()[0];
    int64_t m = blockAType.getShape()[1];
    int64_t n = blockBType.getShape()[1];
    int64_t kPack = blockAType.getShape()[2];

    // Non-xdlops path.

    // Obtain critical attributes.
    int64_t mC = bufferCType.getShape()[0];
    int64_t nC = bufferCType.getShape()[1];
    int64_t kPerThread = op.kPerThreadAttr().getInt();
    int64_t mPerThread = op.mPerThreadAttr().getInt();
    int64_t nPerThread = op.nPerThreadAttr().getInt();
    int64_t mRepeatStride = op.mRepeatStrideAttr().getInt();
    int64_t nRepeatStride = op.nRepeatStrideAttr().getInt();
    int64_t mRepeat = mC / mPerThread;
    int64_t nRepeat = nC / nPerThread;

    LLVM_DEBUG(llvm::dbgs() << "M: " << mC << "\n"
                            << "NRepeat: " << mRepeat << "\n"
                            << "MPerThread: " << mPerThread << "\n"
                            << "N: " << nC << "\n"
                            << "NRepeat: " << nRepeat << "\n"
                            << "NPerThread: " << nPerThread << "\n");

    TopDownTMBuilder strideLDSBufferA(b,
                                      {"k", "mRepeat", "mPerThread", "kpack"},
                                      {k, mRepeat, m / mRepeat, kPack}, loc);
    strideLDSBufferA.passThrough("k");
    strideLDSBufferA.embed("m", 1, m, {"mRepeat", "mPerThread"},
                           {mRepeatStride, 1});
    strideLDSBufferA.passThrough({"kpack"}, {2}, {"kpack"});
    TransformMapAttr strideLDSBufferAAttr = strideLDSBufferA.get();

    TopDownTMBuilder strideLDSBufferB(b,
                                      {"k", "nRepeat", "nPerThread", "kpack"},
                                      {k, nRepeat, n / nRepeat, kPack}, loc);
    strideLDSBufferB.passThrough("k");
    strideLDSBufferB.embed("n", 1, n, {"nRepeat", "nPerThread"},
                           {nRepeatStride, 1});
    strideLDSBufferB.passThrough({"kpack"}, {2}, {"kpack"});
    TransformMapAttr strideLDSBufferBAttr = strideLDSBufferB.get();

    Value matrixA, matrixB;
    ArrayAttr transformsA, transformsB;
    std::tie(matrixA, transformsA) = untransform(
        b, adaptor.matrixA(), b.getArrayAttr({strideLDSBufferAAttr}));
    std::tie(matrixB, transformsB) = untransform(
        b, adaptor.matrixB(), b.getArrayAttr({strideLDSBufferBAttr}));

    int64_t threadANumRegisters = kPerThread * mC * kPack;
    int64_t threadBNumRegisters = kPerThread * nC * kPack;

    // Alloc register for thread_a and thread_b.
    auto threadARegisterMemRefType =
        MemRefType::get(threadANumRegisters, elementType, {},
                        gpu::GPUDialect::getPrivateAddressSpace());
    auto threadAAllocOp = b.create<GpuAllocOp>(loc, threadARegisterMemRefType);

    auto threadBRegisterMemRefType =
        MemRefType::get(threadBNumRegisters, elementType, {},
                        gpu::GPUDialect::getPrivateAddressSpace());
    auto threadBAllocOp = b.create<GpuAllocOp>(loc, threadBRegisterMemRefType);

    // Define views of register tiles for copies
    BottomUpTMBuilder viewA(b, {"raw"}, {threadANumRegisters}, loc);
    viewA.unmerge({"k", "mRepeat", "mPerThread", "kpack"}, {0, 1, 2, 3}, "raw",
                  {kPerThread, mRepeat, mPerThread, kPack});
    TransformMapAttr threadACopyViewAttr = viewA.get();

    BottomUpTMBuilder viewB(b, {"raw"}, {threadBNumRegisters}, loc);
    viewB.unmerge({"k", "nRepeat", "nPerThread", "kpack"}, {0, 1, 2, 3}, "raw",
                  {kPerThread, nRepeat, nPerThread, kPack});
    TransformMapAttr threadBCopyViewAttr = viewB.get();

    // Main loop.
    LLVM_DEBUG(llvm::dbgs() << "Outer loop:\n "
                            << "k =  " << k << "\n"
                            << " kPerThread = " << kPerThread << "\n");
    auto loopOp = b.replaceOpWithNewOp<AffineForOp>(op, 0, k, kPerThread);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(loopOp.getBody());
    Value kOffset = loopOp.getInductionVar();

    SmallVector<Value, 5> registerStartCoords(4, zeroConstantOp);
    SmallVector<Value, 5> ldsBufferAStartCoords = {
        kOffset, zeroConstantOp, op.threadOffsetA(), zeroConstantOp};
    auto copyALoop = b.create<TransformingForOp>(
        loc, ArrayRef<ValueRange>{ldsBufferAStartCoords, registerStartCoords},
        ArrayRef<Attribute>{transformsA, b.getArrayAttr(threadACopyViewAttr)},
        ArrayRef<int64_t>{kPerThread, mRepeat, mPerThread, kPack},
        /*strides=*/llvm::None, /*forceUnroll=*/true, /*indexDiffs=*/true);
    {
      OpBuilder::InsertionGuard copyAGuard(b);
      b.setInsertionPointToStart(copyALoop.getBody());
      Value aCopy = b.create<memref::LoadOp>(
          loc, matrixA, copyALoop.getLowerCoords(/*domain=*/0));
      Value aCast = createTypeConversionOp(b, loc, aCopy, elementType);
      b.create<memref::StoreOp>(loc, aCast, threadAAllocOp,
                                copyALoop.getLowerCoords(/*domain=*/1));
    }

    SmallVector<Value, 5> ldsBufferBStartCoords = {
        kOffset, zeroConstantOp, op.threadOffsetB(), zeroConstantOp};
    auto copyBLoop = b.create<TransformingForOp>(
        loc, ArrayRef<ValueRange>{ldsBufferBStartCoords, registerStartCoords},
        ArrayRef<Attribute>{transformsB, b.getArrayAttr(threadBCopyViewAttr)},
        ArrayRef<int64_t>{kPerThread, nRepeat, nPerThread, kPack},
        /*strides=*/llvm::None, /*forceUnroll=*/true, /*indexDiffs=*/true);
    {
      OpBuilder::InsertionGuard copyBGuard(b);
      b.setInsertionPointToStart(copyBLoop.getBody());
      Value bCopy = b.create<memref::LoadOp>(
          loc, matrixB, copyBLoop.getLowerCoords(/*domain=*/0));
      Value bCast = createTypeConversionOp(b, loc, bCopy, elementType);
      b.create<memref::StoreOp>(loc, bCast, threadBAllocOp,
                                copyBLoop.getLowerCoords(/*domain=*/1));
    }

    Value reshapedARegisters = reshapeBuffer(
        b, loc, threadAAllocOp, {"k", "m", "kpack"}, {kPerThread, mC, kPack});
    Value reshapedBRegisters = reshapeBuffer(
        b, loc, threadBAllocOp, {"k", "n", "kpack"}, {kPerThread, nC, kPack});
    // Actually do the gemm - this goes inside the look over kOffset
    b.create<ThreadwiseGemmOp>(loc, reshapedARegisters, reshapedBRegisters,
                               op.matrixC());

    return success();
  }
};

//===----------------------------------------------------------------------===//
// BlockwiseGemmV2 lowering.
//===----------------------------------------------------------------------===//

struct BlockwiseGemmV2RewritePattern
    : public OpConversionPattern<BlockwiseGemmV2Op> {
  using OpConversionPattern<BlockwiseGemmV2Op>::OpConversionPattern;

  LogicalResult matchAndRewrite(BlockwiseGemmV2Op op,
                                BlockwiseGemmV2OpAdaptor adaptor,
                                ConversionPatternRewriter &b) const override {
    Location loc = op.getLoc();

    int64_t M = op->getAttr("m").template cast<IntegerAttr>().getInt();
    int64_t N = op->getAttr("n").template cast<IntegerAttr>().getInt();
    int64_t K = op->getAttr("k").template cast<IntegerAttr>().getInt();
    int64_t MPerWave =
        op->getAttr("m_per_wave").template cast<IntegerAttr>().getInt();
    int64_t NPerWave =
        op->getAttr("n_per_wave").template cast<IntegerAttr>().getInt();
    int64_t KPack =
        op->hasAttr("kpack")
            ? op->getAttr("kpack").template cast<IntegerAttr>().getInt()
            : 1;

    // Original C++ logic.
    // static constexpr index_t MRepeats = (GemmMPerWave > 64) ? (GemmMPerWave /
    // 64) : 1; static constexpr index_t NRepeats = (GemmNPerWave > 64) ?
    // (GemmNPerWave / 64) : 1; static constexpr index_t MPerXdlops =
    // (GemmMPerWave > 64) ? 64 : GemmMPerWave; static constexpr index_t
    // NPerXdlops = (GemmNPerWave > 64) ? 64 : GemmNPerWave;

    int64_t MRepeats = (MPerWave > 64) ? (MPerWave / 64) : 1;
    int64_t NRepeats = (NPerWave > 64) ? (NPerWave / 64) : 1;
    int64_t MPerXdlops = (MPerWave > 64) ? 64 : MPerWave;
    int64_t NPerXdlops = (NPerWave > 64) ? 64 : NPerWave;

    int64_t ldsOffsetA = op.ldsBufferOffsetA().getSExtValue();
    int64_t ldsOffsetB = op.ldsBufferOffsetB().getSExtValue();

    assert(ldsOffsetA % KPack == 0 &&
           "LDS buffer segment for A is kpack-aligned");
    assert(ldsOffsetB % KPack == 0 &&
           "LDS buffer segment for B is kpack-aligned");
    auto dataType = adaptor.matrixA()
                        .getType()
                        .template cast<MemRefType>()
                        .getElementType();

    // The address calculations into the LDS buffer assume that the buffer
    // has type vector<KPack x T>. Then, we convert that into an address
    // in a buffer of Ts through a final multiplicaiton by KPack.
    // However, the LDS buffer offset, which was computed when the buffer was
    // allocated, is an offset into a buffer of T. Therefore, to allow it to
    // easily participate in adress calculations (instead of adding it on at the
    // end) we must divide it by KPack here. Fortunately, this offset will be
    // KPack-alligned and so this is safe
    Value aBase =
        b.create<AddIOp>(loc, adaptor.waveOffsetA(),
                         b.create<ConstantIndexOp>(loc, ldsOffsetA / KPack));
    Value bBase =
        b.create<AddIOp>(loc, adaptor.waveOffsetB(),
                         b.create<ConstantIndexOp>(loc, ldsOffsetB / KPack));

    XdlopsCodeSelection xcs =
        XdlopsCodeSelection::get(dataType, MPerWave, NPerWave, b);

    // Extract values from XdlopsCodeSelection.
    amdgpu::MFMAInstr mfmaInstr = xcs.instr;
    LLVM_DEBUG(llvm::dbgs() << "Selected xdlop: "
                            << amdgpu::stringifyMFMAInstr(mfmaInstr) << "\n");
    SmallVector<SmallVector<unsigned, 3>, 2> imms = xcs.imms;
    Type argType = xcs.argType;

    int64_t num_threads_blk = xcs.num_threads_blk;
    int64_t num_input_blks = xcs.num_input_blks;
    int64_t num_output_blks = xcs.num_output_blks;
    int64_t k_base = xcs.k_base;

    bool IsKReduction = (num_output_blks == 1) && (num_input_blks > 1);

    if (KPack > 1 && (KPack < k_base || KPack % k_base != 0)) {
      llvm_unreachable(
          "Tuning parameter selection guarantees kPack is multiple of k_base,"
          "this should never happen");
    }

    // const index_t laneId = get_thread_local_1d_id() % mfma_type.wave_size;
    // FloatA a[KPerThread * MRepeats];
    // FloatB b[KPerThread * NRepeats];
    // constexpr index_t KRepeats = KPack / mfma_type.k_base;
    // auto pa = reinterpret_cast<const data_type*>(&a);
    // auto pb = reinterpret_cast<const data_type*>(&b);
    // constexpr index_t AStride = KPerThread * KRepeats;
    // constexpr index_t BStride = KPerThread * KRepeats;

    auto tid = b.create<WorkitemIdOp>(loc, b.getIndexType());
    constexpr int64_t waveSize = 64;
    auto laneId =
        b.create<RemUIOp>(loc, tid, b.create<ConstantIndexOp>(loc, waveSize));

    LLVM_DEBUG(llvm::dbgs()
               << "argVectorType: " << argType << "\n"
               << "k_base: " << k_base << "\n"
               << "K: " << K << "\n"
               << "bufferA type: " << adaptor.bufferA().getType() << "\n"
               << "bufferB type: " << adaptor.bufferB().getType() << "\n");

    auto MConstantOp = b.create<ConstantIndexOp>(loc, M);
    auto NConstantOp = b.create<ConstantIndexOp>(loc, N);
    auto KConstantOp = b.create<ConstantIndexOp>(loc, K);

    auto MPerXdlopsConstantOp = b.create<ConstantIndexOp>(loc, MPerXdlops);
    auto NPerXdlopsConstantOp = b.create<ConstantIndexOp>(loc, NPerXdlops);

    Value bufferA = adaptor.bufferA();
    Value bufferB = adaptor.bufferB();
    auto bufferAType = adaptor.bufferA().getType().cast<MemRefType>();
    auto bufferBType = adaptor.bufferB().getType().cast<MemRefType>();
    Type bufferAElementType = bufferAType.getElementType();
    Type bufferBElementType = bufferBType.getElementType();

    int64_t KPerThread = IsKReduction ? K / num_input_blks : K;
    Value zeroConstantOp = b.createOrFold<ConstantIndexOp>(loc, 0);
    auto KPerBlockConstantOp = b.create<ConstantIndexOp>(loc, KPerThread);

    if (!IsKReduction) {

      // store bufferA logic.
      // for(index_t m_i = 0; m_i < MRepeats; ++m_i)
      //   for(index_t k_i      = 0; k_i < K; ++k_i)
      //     a[k_i + m_i * K] = p_a_wave[k_i * M + laneId + MPerXdlops * m_i];
      // Note: p_a_wave need to be offseted by waveOffsetA.

      auto outerLoopM = b.create<AffineForOp>(loc, 0, MRepeats);
      auto olmb = ConversionPatternRewriter::atBlockBegin(outerLoopM.getBody(),
                                                          b.getListener());
      auto olmiv = outerLoopM.getInductionVar();
      auto mOffset = olmb.create<AddIOp>(
          loc, aBase, olmb.create<MulIOp>(loc, MPerXdlopsConstantOp, olmiv));
      auto kOffsetA = olmb.create<MulIOp>(loc, olmiv, KConstantOp);

      auto innerLoopMK = olmb.create<AffineForOp>(loc, 0, KPerThread);
      auto ilmkb = ConversionPatternRewriter::atBlockBegin(
          innerLoopMK.getBody(), olmb.getListener());
      auto ilmkiv = innerLoopMK.getInductionVar();

      Value sourceOffsetA = ilmkb.create<AddIOp>(
          loc,
          ilmkb.create<AddIOp>(
              loc, ilmkb.create<MulIOp>(loc, ilmkiv, MConstantOp), laneId),
          mOffset);

      if (KPack > 1)
        sourceOffsetA = ilmkb.create<MulIOp>(
            loc, sourceOffsetA, ilmkb.create<ConstantIndexOp>(loc, KPack));

      auto destOffsetA = ilmkb.create<AddIOp>(loc, ilmkiv, kOffsetA);

      Value valueA = ilmkb.create<InBoundsLoadOp>(loc, bufferAElementType,
                                                  op.matrixA(), sourceOffsetA);
      ilmkb.create<memref::StoreOp>(loc, valueA, bufferA,
                                    ValueRange{destOffsetA});

      // store bufferB logic.
      // for(index_t n_i = 0; n_i < NRepeats; ++n_i)
      //   for(index_t k_i      = 0; k_i < KPerThread; ++k_i)
      //     b[k_i + n_i * KPerThread] = p_b_wave[k_i * N + laneId + NPerXdlops
      //     * n_i];
      // Note: p_b_wave need to be offseted by waveOffsetB.

      auto outerLoopN = b.create<AffineForOp>(loc, 0, NRepeats);
      auto olnb = ConversionPatternRewriter::atBlockBegin(outerLoopN.getBody(),
                                                          b.getListener());
      auto olniv = outerLoopN.getInductionVar();
      auto nOffset = olnb.create<AddIOp>(
          loc, bBase, olnb.create<MulIOp>(loc, NPerXdlopsConstantOp, olniv));
      auto kOffsetB = olnb.create<MulIOp>(loc, olniv, KConstantOp);

      auto innerLoopNK = olnb.create<AffineForOp>(loc, 0, KPerThread);
      auto ilnkb = ConversionPatternRewriter::atBlockBegin(
          innerLoopNK.getBody(), olnb.getListener());
      auto ilnkiv = innerLoopNK.getInductionVar();

      Value sourceOffsetB = ilnkb.create<AddIOp>(
          loc,
          ilnkb.create<AddIOp>(
              loc, ilnkb.create<MulIOp>(loc, ilnkiv, NConstantOp), laneId),
          nOffset);

      if (KPack > 1)
        sourceOffsetB = ilnkb.create<MulIOp>(
            loc, sourceOffsetB, ilnkb.create<ConstantIndexOp>(loc, KPack));

      auto destOffsetB = ilnkb.create<AddIOp>(loc, ilnkiv, kOffsetB);

      Value valueB = ilnkb.create<InBoundsLoadOp>(loc, bufferBElementType,
                                                  op.matrixB(), sourceOffsetB);
      ilnkb.create<memref::StoreOp>(loc, valueB, bufferB,
                                    ValueRange{destOffsetB});
    } else {
      // const index_t blk_id = laneId / mfma_type.num_threads_blk;
      // const index_t blk_td = laneId % mfma_type.num_threads_blk;
      auto NumThreadsBlkConstantOp =
          b.create<ConstantIndexOp>(loc, num_threads_blk);
      auto blk_id = b.create<DivUIOp>(loc, laneId, NumThreadsBlkConstantOp);
      auto blk_td = b.create<RemUIOp>(loc, laneId, NumThreadsBlkConstantOp);

      Value kBaseA = b.create<AddIOp>(loc, aBase, blk_td);
      Value kBaseB = b.create<AddIOp>(loc, bBase, blk_td);

      // for(index_t k_i = 0; k_i < KPerThread; k_i += mfma_type.num_input_blks)
      // {
      //     a[k_i] = p_a_wave[(k_i * num_input_blks + blk_id) * M + blk_td];
      //     b[k_i] = p_b_wave[(k_i * num_input_blks + blk_id) * N + blk_td];
      // }
      // p_a_wave need to be offseted by waveOffsetA.
      // p_b_wave need to be offseted by waveOffsetB.

      auto NumInputBlksConstantOp =
          b.create<ConstantIndexOp>(loc, num_input_blks);

      auto loopKLoad = b.create<AffineForOp>(loc, 0, KPerThread);
      auto lklb = ConversionPatternRewriter::atBlockBegin(loopKLoad.getBody(),
                                                          b.getListener());
      auto lkliv = loopKLoad.getInductionVar();

      Value sourceOffsetA = lklb.create<AddIOp>(
          loc,
          lklb.create<MulIOp>(
              loc,
              lklb.create<AddIOp>(
                  loc, lklb.create<MulIOp>(loc, lkliv, NumInputBlksConstantOp),
                  blk_id),
              MConstantOp),
          kBaseA);

      if (KPack > 1)
        sourceOffsetA = lklb.create<MulIOp>(
            loc, sourceOffsetA, lklb.create<ConstantIndexOp>(loc, KPack));

      Value valueA = lklb.create<InBoundsLoadOp>(loc, bufferAElementType,
                                                 op.matrixA(), sourceOffsetA);
      lklb.create<memref::StoreOp>(loc, valueA, bufferA, ValueRange{lkliv});

      Value sourceOffsetB = lklb.create<AddIOp>(
          loc,
          lklb.create<MulIOp>(
              loc,
              lklb.create<AddIOp>(
                  loc, lklb.create<MulIOp>(loc, lkliv, NumInputBlksConstantOp),
                  blk_id),
              NConstantOp),
          kBaseB);

      if (KPack > 1)
        sourceOffsetB = lklb.create<MulIOp>(
            loc, sourceOffsetB, lklb.create<ConstantIndexOp>(loc, KPack));

      Value valueB = lklb.create<InBoundsLoadOp>(loc, bufferBElementType,
                                                 op.matrixB(), sourceOffsetB);
      lklb.create<memref::StoreOp>(loc, valueB, bufferB, ValueRange{lkliv});
    }

    if (MRepeats == 1 && NRepeats == 1) {
      SmallVector<Type, 2> resultTypes;
      for (auto result : op.vectorDs()) {
        resultTypes.push_back(result.getType());
      }

      auto xdlopsGemmV2Op = b.replaceOpWithNewOp<XdlopsGemmV2Op>(
          op, resultTypes, adaptor.matrixA(), adaptor.matrixB(),
          op.ldsBufferOffsetA(), op.ldsBufferOffsetB(), zeroConstantOp,
          zeroConstantOp, adaptor.bufferA(), adaptor.bufferB(),
          adaptor.vectorCs());

      xdlopsGemmV2Op->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op->setAttr("k", op->getAttr("k"));
      xdlopsGemmV2Op->setAttr("m_per_wave", op->getAttr("m_per_wave"));
      xdlopsGemmV2Op->setAttr("n_per_wave", op->getAttr("n_per_wave"));
      if (op->hasAttr("kpack"))
        xdlopsGemmV2Op->setAttr("kpack", op->getAttr("kpack"));
    } else if (MRepeats == 2 && NRepeats == 1) {
      // Original C++ logic.
      // p_c_thread.s.x.l = XdlopsGemm.template Run<M, N, K>(
      // p_a_block, p_b_block, p_c_thread.s.x.l);
      // p_c_thread.s.y.l = XdlopsGemm.templateRun<M, N, K>(
      // p_a_block + MPerXdlops, p_b_block, p_c_thread.s.y.l);

      SmallVector<Type, 2> resultTypes0;
      resultTypes0.push_back(op.vectorDs()[0].getType());
      resultTypes0.push_back(op.vectorDs()[1].getType());

      auto xdlopsGemmV2Op0 = b.create<XdlopsGemmV2Op>(
          loc, resultTypes0, adaptor.matrixA(), adaptor.matrixB(),
          op.ldsBufferOffsetA(), op.ldsBufferOffsetB(), zeroConstantOp,
          zeroConstantOp, adaptor.bufferA(), adaptor.bufferB(),
          adaptor.vectorCs().take_front(2));

      xdlopsGemmV2Op0->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op0->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op0->setAttr("k", op->getAttr("k"));
      // Hard-coded m_per_wave/n_per_wave as 64 when MRepeat>1 or NRepeat>1.
      // So each xdlops_gemm_v2 handles a 64x64 GEMM.
      xdlopsGemmV2Op0->setAttr("m_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op0->setAttr("n_per_wave", b.getI32IntegerAttr(64));
      if (op->hasAttr("kpack"))
        xdlopsGemmV2Op0->setAttr("kpack", op->getAttr("kpack"));

      SmallVector<Type, 2> resultTypes1;
      resultTypes1.push_back(op.vectorDs()[2].getType());
      resultTypes1.push_back(op.vectorDs()[3].getType());

      auto xdlopsGemmV2Op1 = b.create<XdlopsGemmV2Op>(
          loc, resultTypes1, adaptor.matrixA(), adaptor.matrixB(),
          op.ldsBufferOffsetA(), op.ldsBufferOffsetB(), KPerBlockConstantOp,
          zeroConstantOp, adaptor.bufferA(), adaptor.bufferB(),
          adaptor.vectorCs().drop_front(2));

      xdlopsGemmV2Op1->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op1->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op1->setAttr("k", op->getAttr("k"));
      // Hard-coded m_per_wave/n_per_wave as 64 when MRepeat>1 or NRepeat>1.
      // So each xdlops_gemm_v2 handles a 64x64 GEMM.
      xdlopsGemmV2Op1->setAttr("m_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op1->setAttr("n_per_wave", b.getI32IntegerAttr(64));
      if (op->hasAttr("kpack"))
        xdlopsGemmV2Op1->setAttr("kpack", op->getAttr("kpack"));

      b.replaceOp(op, ValueRange{xdlopsGemmV2Op0.vectorDs()[0],
                                 xdlopsGemmV2Op0.vectorDs()[1],
                                 xdlopsGemmV2Op1.vectorDs()[0],
                                 xdlopsGemmV2Op1.vectorDs()[1]});
    } else if (MRepeats == 1 && NRepeats == 2) {
      // Original C++ logic.
      // p_c_thread.s.x.l = XdlopsGemm.template Run<M, N, K>(
      // p_a_block, p_b_block, p_c_thread.s.x.l);
      // p_c_thread.s.y.l = XdlopsGemm.template Run<M, N, K>(
      // p_a_block, p_b_block + NPerXdlops, p_c_thread.s.y.l);

      SmallVector<Type, 2> resultTypes0;
      resultTypes0.push_back(op.vectorDs()[0].getType());
      resultTypes0.push_back(op.vectorDs()[1].getType());

      auto xdlopsGemmV2Op0 = b.create<XdlopsGemmV2Op>(
          loc, resultTypes0, adaptor.matrixA(), adaptor.matrixB(),
          op.ldsBufferOffsetA(), op.ldsBufferOffsetB(), zeroConstantOp,
          zeroConstantOp, adaptor.bufferA(), adaptor.bufferB(),
          adaptor.vectorCs().take_front(2));

      xdlopsGemmV2Op0->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op0->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op0->setAttr("k", op->getAttr("k"));
      // Hard-coded m_per_wave/n_per_wave as 64 when MRepeat>1 or NRepeat>1.
      // So each xdlops_gemm_v2 handles a 64x64 GEMM.
      xdlopsGemmV2Op0->setAttr("m_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op0->setAttr("n_per_wave", b.getI32IntegerAttr(64));
      if (op->hasAttr("kpack"))
        xdlopsGemmV2Op0->setAttr("kpack", op->getAttr("kpack"));

      SmallVector<Type, 2> resultTypes1;
      resultTypes1.push_back(op.vectorDs()[2].getType());
      resultTypes1.push_back(op.vectorDs()[3].getType());

      auto xdlopsGemmV2Op1 = b.create<XdlopsGemmV2Op>(
          loc, resultTypes1, adaptor.matrixA(), adaptor.matrixB(),
          op.ldsBufferOffsetA(), op.ldsBufferOffsetB(), zeroConstantOp,
          KPerBlockConstantOp, adaptor.bufferA(), adaptor.bufferB(),
          adaptor.vectorCs().drop_front(2));

      xdlopsGemmV2Op1->setAttr("m", op->getAttr("m"));
      xdlopsGemmV2Op1->setAttr("n", op->getAttr("n"));
      xdlopsGemmV2Op1->setAttr("k", op->getAttr("k"));
      // Hard-coded m_per_wave/n_per_wave as 64 when MRepeat>1 or NRepeat>1.
      // So each xdlops_gemm_v2 handles a 64x64 GEMM.
      xdlopsGemmV2Op1->setAttr("m_per_wave", b.getI32IntegerAttr(64));
      xdlopsGemmV2Op1->setAttr("n_per_wave", b.getI32IntegerAttr(64));
      if (op->hasAttr("kpack"))
        xdlopsGemmV2Op1->setAttr("kpack", op->getAttr("kpack"));

      b.replaceOp(op, ValueRange{xdlopsGemmV2Op0.vectorDs()[0],
                                 xdlopsGemmV2Op0.vectorDs()[1],
                                 xdlopsGemmV2Op1.vectorDs()[0],
                                 xdlopsGemmV2Op1.vectorDs()[1]});
    }

    return success();
  }
};

//===----------------------------------------------------------------------===//
// ThreadwiseCopyV2 lowering.
//===----------------------------------------------------------------------===//
struct ThreadwiseCopyV2RewritePattern
    : public OpRewritePattern<ThreadwiseCopyV2Op> {
  using OpRewritePattern<ThreadwiseCopyV2Op>::OpRewritePattern;
  LogicalResult matchAndRewrite(ThreadwiseCopyV2Op op,
                                PatternRewriter &b) const override {
    Location loc = op.getLoc();

    Value source = op.source();
    auto sourceType = source.getType().cast<MemRefType>();
    Value sourceCoord = op.sourceCoord();

    int64_t copyLength = op.length().getSExtValue();
    Type typeToLoad = sourceType.getElementType();
    if (copyLength > 1)
      typeToLoad = VectorType::get({copyLength}, typeToLoad);
    Type typeToStore = op.dest().getType().cast<MemRefType>().getElementType();
    if (copyLength > 1)
      typeToStore = VectorType::get({copyLength}, typeToStore);

    Value loaded =
        b.create<InBoundsLoadOp>(loc, typeToLoad, source, sourceCoord);
    b.replaceOpWithNewOp<BufferStoreOp>(op, loaded, op.dest(), op.leftOobDims(),
                                        op.rightOobDims(), op.destCoord(),
                                        op.storeMethodAttr());
    return success();
  }
};

void MIOpenLowerBlockwiseGemmToThreadwisePass::runOnOperation() {
  MLIRContext *ctx = &getContext();
  ConversionTarget target(*ctx);
  target.addIllegalOp<FillOp, BlockwiseGemmOp, BlockwiseGemmV2Op,
                      ThreadwiseCopyV2Op>();
  target.addLegalDialect<arith::ArithmeticDialect, miopen::MIOpenDialect,
                         AffineDialect, memref::MemRefDialect,
                         vector::VectorDialect>();

  RewritePatternSet patterns(ctx);
  patterns.add<FillRewritePattern, BlockwiseGemmRewritePattern,
               BlockwiseGemmV2RewritePattern, ThreadwiseCopyV2RewritePattern>(
      ctx);
  if (failed(
          applyPartialConversion(getOperation(), target, std::move(patterns))))
    signalPassFailure();
}
} // end anonymous namespace

std::unique_ptr<Pass>
mlir::miopen::createMIOpenBlockwiseGemmToThreadwisePass() {
  return std::make_unique<MIOpenLowerBlockwiseGemmToThreadwisePass>();
}
