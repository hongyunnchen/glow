// Copyright 2017 Facebook Inc.  All Rights Reserved.

#include "glow/IR/Instrs.h"
#include "glow/Interpreter/Interpreter.h"
#include "glow/Support/Casting.h"

using namespace glow;

//===----------------------------------------------------------------------===//
//                       Convolution
//===----------------------------------------------------------------------===//

void Interpreter::fwdCopyInst(Context *ctx, bool isTrain, const CopyInst *I) {
  auto S = getWeightHandle(ctx, I->getSrc());
  auto D = getWeightHandle(ctx, I->getDest());

  for (size_t i = 0, e = S.size(); i < e; i++) {
    D.raw(i) = S.raw(i);
  }
}

void Interpreter::bwdCopyInst(Context *ctx, const CopyInst *I) {
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outG = getGradHandle(ctx, I->getDest());

  for (size_t i = 0, e = outG.size(); i < e; i++) {
    inG.raw(i) += outG.raw(i);
  }
}

void Interpreter::fwdConvolutionInst(Context *ctx, bool isTrain,
                                     const ConvolutionInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto filterW = getWeightHandle(ctx, I->getFilter());
  auto biasW = getWeightHandle(ctx, I->getBias());

  size_t filterSize = I->getKernel();
  size_t pad = I->getPad();
  size_t stride = I->getStride();

  ShapeNHWC odim(outW.dims());
  ShapeNHWC idim(inW.dims());

  // For each input in the batch:
  for (size_t n = 0; n < idim.n; n++) {

    // For each layer in the output tensor:
    for (size_t d = 0; d < odim.c; d++) {

      // For each convolution 'jump' in the input tensor:
      ssize_t y = -ssize_t(pad);
      for (size_t ay = 0; ay < odim.w; y += stride, ay++) {
        ssize_t x = -ssize_t(pad);
        for (size_t ax = 0; ax < odim.h; x += stride, ax++) {

          // For each element in the convolution-filter:
          FloatTy sum = 0;
          for (size_t fy = 0; fy < filterSize; fy++) {
            for (size_t fx = 0; fx < filterSize; fx++) {
              ssize_t ox = x + fx;
              ssize_t oy = y + fy;

              // Ignore index access below zero (this is due to padding).
              if (ox < 0 || oy < 0 || ox >= ssize_t(odim.h) ||
                  oy >= ssize_t(odim.w)) {
                continue;
              }

              for (size_t fd = 0; fd < idim.c; fd++) {
                sum += filterW.at({d, fx, fy, fd}) *
                       inW.at({n, (size_t)ox, (size_t)oy, fd});
              }
            }
          }

          sum += biasW.at({d});
          outW.at({n, ax, ay, d}) = sum;
        } // H
      }   // W
    }     // C
  }       // N
}

void Interpreter::bwdConvolutionInst(Context *ctx, const ConvolutionInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto outG = getGradHandle(ctx, I->getDest());

  auto filterW = getWeightHandle(ctx, I->getFilter());
  auto filterG = getGradHandle(ctx, I->getFilter());
  auto biasG = getGradHandle(ctx, I->getBias());

  size_t filterSize = I->getKernel();
  size_t pad = I->getPad();
  size_t stride = I->getStride();

  ShapeNHWC odim(outW.dims());
  ShapeNHWC idim(inW.dims());

  // For each input in the batch:
  for (size_t n = 0; n < odim.n; n++) {

    // Compute the gradient. For each layer in the output tensor:
    for (size_t d = 0; d < odim.c; d++) {

      // For each convolution 'jump' in the input tensor:
      ssize_t y = -ssize_t(pad);
      for (size_t ay = 0; ay < odim.w; y += stride, ay++) {
        ssize_t x = -ssize_t(pad);
        for (size_t ax = 0; ax < odim.h; x += stride, ax++) {
          FloatTy chainGrad = outG.at({n, ax, ay, d});

          // For each element in the convolution-filter:
          for (size_t fy = 0; fy < filterSize; fy++) {
            for (size_t fx = 0; fx < filterSize; fx++) {
              ssize_t ox = x + fx;
              ssize_t oy = y + fy;

              // Ignore index access below zero (this is due to padding).
              if (ox < 0 || oy < 0 || ox >= ssize_t(odim.h) ||
                  oy >= ssize_t(odim.w)) {
                continue;
              }

              for (size_t fd = 0; fd < idim.c; fd++) {
                filterG.at({d, fx, fy, fd}) +=
                    inW.at({0u, (size_t)ox, (size_t)oy, fd}) * chainGrad;
                inG.at({n, (size_t)ox, (size_t)oy, fd}) +=
                    filterW.at({d, fx, fy, fd}) * chainGrad;
              }
            }
          }

          biasG.at({d}) += chainGrad;
        } // H
      }   // W
    }     // C
  }       // N
}

//===----------------------------------------------------------------------===//
//                       Pooling
//===----------------------------------------------------------------------===//

void Interpreter::fwdPoolInst(Context *ctx, bool isTrain, const PoolInst *I) {
  if (I->getKind() == PoolInst::OpKind::Max) {
    return fwdPoolMax_impl(ctx, I);
  }

  return fwdPoolAvg_impl(ctx, I);
}

void Interpreter::fwdPoolMax_impl(Context *ctx, const PoolInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());

  ShapeNHWC odim(outW.dims());
  ShapeNHWC idim(inW.dims());

  auto pad = I->getPad();
  auto filterSize = I->getKernel();
  auto stride = I->getStride();

  auto SXY = getTensorForValue(I->srcXY())->getHandle<size_t>();

  // For each input in the batch:
  for (size_t n = 0; n < odim.n; n++) {

    // For each layer in the output tensor:
    for (size_t z = 0; z < idim.c; z++) {
      // For each convolution 'jump' in the input tensor:
      ssize_t y = -ssize_t(pad);
      for (size_t ay = 0; ay < odim.w; y += stride, ay++) {
        ssize_t x = -ssize_t(pad);
        for (size_t ax = 0; ax < odim.h; x += stride, ax++) {
          size_t maxX = x;
          size_t maxY = y;

          bool first = true;
          FloatTy max = 0;

          for (size_t fy = 0; fy < filterSize; fy++) {
            for (size_t fx = 0; fx < filterSize; fx++) {
              ssize_t ox = x + fx;
              ssize_t oy = y + fy;

              // Ignore index access below zero (this is due to padding).
              if (ox < 0 || oy < 0 || ox >= ssize_t(idim.h) ||
                  oy >= ssize_t(idim.w)) {
                continue;
              }

              FloatTy val = inW.at({n, (size_t)ox, (size_t)oy, z});

              if (first || (val >= max)) {
                first = false;
                max = val;
                maxX = ox;
                maxY = oy;
              }
            }
          }

          assert(!first && "Max value is uninitialized");
          SXY.at({n, ax, ay, z, 0}) = maxX;
          SXY.at({n, ax, ay, z, 1}) = maxY;
          outW.at({n, ax, ay, z}) = max;
        } // H
      }   // W
    }     // C
  }       // N
}

void Interpreter::fwdPoolAvg_impl(Context *ctx, const PoolInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());

  ShapeNHWC odim(outW.dims());
  ShapeNHWC idim(inW.dims());

  auto pad = I->getPad();
  auto filterSize = I->getKernel();
  auto stride = I->getStride();

  // Implement the avg pooling operation as defined here:
  // https://arxiv.org/abs/1312.4400

  FloatTy filterArea = filterSize * filterSize;

  // For each input in the batch:
  for (size_t n = 0; n < odim.n; n++) {

    // For each layer in the output tensor:
    for (size_t z = 0; z < idim.c; z++) {
      // For each convolution 'jump' in the input tensor:
      ssize_t y = -ssize_t(pad);
      for (size_t ay = 0; ay < odim.w; y += stride, ay++) {
        ssize_t x = -ssize_t(pad);
        for (size_t ax = 0; ax < odim.h; x += stride, ax++) {
          FloatTy sum = 0;

          for (size_t fy = 0; fy < filterSize; fy++) {
            for (size_t fx = 0; fx < filterSize; fx++) {
              ssize_t ox = x + fx;
              ssize_t oy = y + fy;

              // Ignore index access below zero (this is due to padding).
              if (ox < 0 || oy < 0 || ox >= ssize_t(idim.h) ||
                  oy >= ssize_t(idim.w)) {
                continue;
              }

              sum += inW.at({n, (size_t)ox, (size_t)oy, z});
            }
          }
          outW.at({n, ax, ay, z}) = sum / filterArea;
        } // H
      }   // W
    }     // C
  }       // N
}

void Interpreter::bwdPoolInst(Context *ctx, const PoolInst *I) {
  if (I->getKind() == PoolInst::OpKind::Max) {
    return bwdPoolMax_impl(ctx, I);
  }

  return bwdPoolAvg_impl(ctx, I);
}

void Interpreter::bwdPoolMax_impl(Context *ctx, const PoolInst *I) {
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto outG = getGradHandle(ctx, I->getDest());

  ShapeNHWC odim(outW.dims());

  auto SXY = getTensorForValue(I->srcXY())->getHandle<size_t>();

  // For each input in the batch:
  for (size_t n = 0; n < odim.n; n++) {

    // Compute the gradient. For each layer in the output tensor:
    for (size_t z = 0; z < odim.c; z++) {

      // For each convolution 'jump' in the input tensor:
      for (size_t ay = 0; ay < odim.w; ay++) {
        for (size_t ax = 0; ax < odim.h; ax++) {

          FloatTy chainGrad = outG.at({n, ax, ay, z});

          size_t maxX = SXY.at({n, ax, ay, z, 0});
          size_t maxY = SXY.at({n, ax, ay, z, 1});

          inG.at({n, maxX, maxY, z}) += chainGrad;
        } // H
      }   // W
    }     // C
  }       // N
}

void Interpreter::bwdPoolAvg_impl(Context *ctx, const PoolInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto outG = getGradHandle(ctx, I->getDest());

  ShapeNHWC odim(outW.dims());
  ShapeNHWC idim(inW.dims());

  auto pad = I->getPad();
  auto filterSize = I->getKernel();
  auto stride = I->getStride();

  FloatTy filterArea = filterSize * filterSize;

  // For each input in the batch:
  for (size_t n = 0; n < odim.n; n++) {

    // For each layer in the output tensor:
    for (size_t z = 0; z < odim.c; z++) {
      // For each convolution 'jump' in the input tensor:
      ssize_t y = -ssize_t(pad);
      for (size_t ay = 0; ay < odim.w; y += stride, ay++) {
        ssize_t x = -ssize_t(pad);
        for (size_t ax = 0; ax < odim.h; x += stride, ax++) {
          FloatTy dy = outG.at({n, ax, ay, z}) / filterArea;

          for (size_t fy = 0; fy < filterSize; fy++) {
            for (size_t fx = 0; fx < filterSize; fx++) {
              ssize_t ox = x + fx;
              ssize_t oy = y + fy;

              // Ignore index access below zero (this is due to padding).
              if (ox < 0 || oy < 0 || ox >= ssize_t(idim.h) ||
                  oy >= ssize_t(idim.w)) {
                continue;
              }
              inG.at({n, (size_t)ox, (size_t)oy, z}) += dy;
            }
          }
        } // H
      }   // W
    }     // C
  }       // N
}

//===----------------------------------------------------------------------===//
//                       Fully Connected
//===----------------------------------------------------------------------===//

void Interpreter::fwdFullyConnectedInst(Context *ctx, const bool isTrain,
                                        const FullyConnectedInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());

  auto odim = flattenCdr(outW.dims());
  auto idim = flattenCdr(inW.dims());

  auto filterW = getWeightHandle(ctx, I->getFilter());
  auto biasW = getWeightHandle(ctx, I->getBias());

  size_t inputSize = idim.second;

  for (size_t n = 0; n < odim.first; n++) {
    size_t base = inW.getElementPtr({n});

    for (size_t i = 0; i < odim.second; i++) {

      FloatTy sum = 0;
      for (size_t j = 0; j < inputSize; j++) {
        sum += inW.raw(base + j) * filterW.at({i, j});
      }

      sum += biasW.at({i});
      outW.at({n, i}) = sum;
    }
  } // N
}

void Interpreter::bwdFullyConnectedInst(Context *ctx,
                                        const FullyConnectedInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto outG = getGradHandle(ctx, I->getDest());

  auto odim = flattenCdr(outW.dims());
  auto idim = flattenCdr(inW.dims());

  auto filterW = getWeightHandle(ctx, I->getFilter());
  auto filterG = getGradHandle(ctx, I->getFilter());
  auto biasG = getGradHandle(ctx, I->getBias());

  size_t inSize = idim.second;

  for (size_t n = 0; n < odim.first; n++) {
    size_t base = inW.getElementPtr({n});

    // Compute the gradient:
    for (size_t i = 0; i < odim.second; i++) {
      FloatTy chainGrad = outG.at({n, i});

      for (size_t j = 0, e = inSize; j < e; j++) {
        // Input gradient:
        inG.raw(base + j) += filterW.at({i, j}) * chainGrad;
        // Param gradient:
        filterG.at({i, j}) += inW.raw(base + j) * chainGrad;
      }

      biasG.at({i}) += chainGrad;
    }
  } // N
}

//===----------------------------------------------------------------------===//
//                       Activation functions
//===----------------------------------------------------------------------===//

void Interpreter::fwdReluInst(Context *ctx, bool isTrain, const ReluInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());

  for (size_t i = 0, e = inW.size(); i < e; i++) {
    FloatTy val = inW.raw(i);
    outW.raw(i) = val < 0 ? 0 : val;
  }
}

void Interpreter::bwdReluInst(Context *ctx, const ReluInst *I) {
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto outG = getGradHandle(ctx, I->getDest());

  for (size_t i = 0, e = outW.size(); i < e; i++) {
    FloatTy val = outW.raw(i);
    inG.raw(i) += (val <= 0 ? 0 : outG.raw(i));
  }
}

void Interpreter::fwdSigmoidInst(Context *ctx, bool isTrain,
                                 const SigmoidInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());

  for (size_t i = 0, e = outW.size(); i < e; i++) {
    FloatTy val = inW.raw(i);
    outW.raw(i) = 1 / (1 + std::exp(-val));
  }
}
void Interpreter::bwdSigmoidInst(Context *ctx, const SigmoidInst *I) {
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto outG = getGradHandle(ctx, I->getDest());

  for (size_t i = 0, e = outW.size(); i < e; i++) {
    FloatTy val = outW.raw(i);
    inG.raw(i) += val * (1 - val) * outG.raw(i);
  }
}

void Interpreter::fwdTanhInst(Context *ctx, bool isTrain, const TanhInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());

  for (size_t i = 0, e = inW.size(); i < e; i++) {
    FloatTy val = inW.raw(i);
    FloatTy exp_val = std::exp(val);
    FloatTy exp_neg_val = std::exp(-val);
    outW.raw(i) = (exp_val - exp_neg_val) / (exp_val + exp_neg_val);
  }
}
void Interpreter::bwdTanhInst(Context *ctx, const TanhInst *I) {
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto outG = getGradHandle(ctx, I->getDest());

  for (size_t i = 0, e = outW.size(); i < e; i++) {
    FloatTy val = outW.raw(i);
    inG.raw(i) += (1 - val * val) * outG.raw(i);
  }
}

//===----------------------------------------------------------------------===//
//                        Loss Functions (Softmax/regression/...)
//===----------------------------------------------------------------------===//

void Interpreter::fwdSoftMaxInst(Context *ctx, bool isTrain,
                                 const SoftMaxInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto idim = inW.dims();

  auto EH = getWeightHandle(ctx, I->getE());

  for (size_t n = 0; n < idim[0]; n++) {
    FloatTy max = inW.at({n, 0});

    // Find Max.
    for (size_t i = 0; i < idim[1]; i++) {
      max = std::max(max, inW.at({n, i}));
    }

    FloatTy sum = 0;

    // Compute exp.
    for (size_t i = 0; i < idim[1]; i++) {
      FloatTy e = std::exp(inW.at({n, i}) - max);
      sum += e;
      EH.at({n, i}) = e;
    }

    // Normalize the output.
    for (size_t i = 0; i < idim[1]; i++) {
      EH.at({n, i}) /= sum;
      outW.at({n, i}) = EH.at({n, i});
    }
  } // N
}

void Interpreter::bwdSoftMaxInst(Context *ctx, const SoftMaxInst *I) {
  auto inG = getGradHandle(ctx, I->getSrc());

  auto idim = inG.dims();
  auto EH = getTensorForValue(I->getE())->getHandle<FloatTy>();
  auto selectedH = getTensorForValue(I->getSelected())->getHandle<size_t>();

  // http://eli.thegreenplace.net/2016/the-softmax-function-and-its-derivative/
  // https://stats.stackexchange.com/questions/79454/softmax-layer-in-a-neural-network
  for (size_t n = 0; n < idim[0]; n++) {
    for (size_t i = 0; i < idim[1]; i++) {
      FloatTy delta = (selectedH.at({n, 0}) == i);
      FloatTy sigma = (EH.at({n, i}) - delta);
      inG.at({n, i}) += sigma;
    }
  }
}

void Interpreter::fwdRegressionInst(Context *ctx, bool isTrain,
                                    const RegressionInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());

  for (size_t i = 0, e = inW.size(); i < e; i++) {
    outW.raw(i) = inW.raw(i);
  }
}

void Interpreter::bwdRegressionInst(Context *ctx, const RegressionInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto inG = getGradHandle(ctx, I->getSrc());
  auto expected = getTensorForValue(I->getExpected());

  auto idim = inW.dims();
  assert(idim.size() == 2 && "Input is expected to be a vector per input");

  auto e = expected->getHandle<FloatTy>();

  // For each input in the batch:
  for (size_t n = 0; n < idim[0]; n++) {

    for (size_t i = 0; i < idim[1]; i++) {
      FloatTy dy = inW.at({n, i}) - e.at({n, i});
      inG.at({n, i}) += dy;
    }
  } // N
}

//===----------------------------------------------------------------------===//
//                       Tensor shape (transpose/reshape/concat/...)
//===----------------------------------------------------------------------===//

void Interpreter::fwdTransposeInst(Context *ctx, bool isTrain,
                                   const TransposeInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getTensorForValue(I->getDest());

  assert(outW->size() == inW.size() && "Invalid tensor dimensions");
  inW.transpose(outW, I->getShuffle());
}

void Interpreter::bwdTransposeInst(Context *ctx, const TransposeInst *I) {
  auto inG = getOrCreateGradTensor(I->getSrc());
  auto outG = getGradHandle(ctx, I->getDest());

  assert(outG.size() == inG->size() && "Invalid tensor dimensions");

  // Generate the reverse shuffle.
  auto shuffle = I->getShuffle();
  std::vector<unsigned> reverseShuffle = shuffle.vec();
  for (unsigned int i = 0; i < shuffle.size(); i++) {
    reverseShuffle[shuffle[i]] = i;
  }

  // Perform the reverse transpsose.
  // TODO: this wipes out the gradients and may cause a bug for operators with
  // multiple users.
  outG.transpose(inG, reverseShuffle);
}

void Interpreter::fwdReshapeInst(Context *ctx, bool isTrain,
                                 const ReshapeInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  for (size_t i = 0, e = inW.size(); i < e; i++) {
    outW.raw(i) = inW.raw(i);
  }
}
void Interpreter::bwdReshapeInst(Context *ctx, const ReshapeInst *I) {
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto outG = getGradHandle(ctx, I->getDest());
  for (size_t i = 0, e = outW.size(); i < e; i++) {
    inG.raw(i) += outG.raw(i);
  }
}

void Interpreter::fwdConcatInst(Context *ctx, bool isTrain,
                                const ConcatInst *I) {
  auto outW = getWeightHandle(ctx, I->getDest());

  // Insert the tensors at this coordinate. Start at zero.
  std::vector<size_t> offset(outW.size(), 0);
  auto dim = I->getDim();

  for (unsigned i = 1, e = I->getNumOperands(); i < e; i++) {
    auto inW = getWeightHandle(ctx, I->getOperand(i).first);

    // Insert the tensor.
    outW.insertTensors(inW, offset);

    // The next tensor starts after this one ends.
    offset[dim] += inW.dims()[dim];
  }
}
void Interpreter::bwdConcatInst(Context *ctx, const ConcatInst *I) {
  auto outG = getGradHandle(ctx, I->getDest());

  // Insert the tensors at this coordinate. Start at zero.
  std::vector<size_t> offset(outG.size(), 0);

  auto dim = I->getDim();

  for (unsigned i = 1, e = I->getNumOperands(); i < e; i++) {
    auto inG = getGradHandle(ctx, I->getOperand(i).first);

    // Insert the tensor.
    outG.extractTensors(inG, offset);

    // TODO: this code assumes that input[i] has only one user, because it
    // zeros the gradient before extracting the tensor.

    // The next tensor starts after this one ends.
    offset[dim] += inG.dims()[dim];
  }
}

//===----------------------------------------------------------------------===//
//                      Batch Normalization
//===----------------------------------------------------------------------===//

void Interpreter::fwdBatchNormalizationInst(Context *ctx, bool isTrain,
                                            const BatchNormalizationInst *I) {
  if (isTrain) {
    return fwdBatchNormalizationInst_train(ctx, I);
  }

  return fwdBatchNormalizationInst_infer(ctx, I);
}

void Interpreter::fwdBatchNormalizationInst_infer(
    Context *ctx, const BatchNormalizationInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());

  auto betaWH = getWeightHandle(ctx, I->getBias());
  auto gammaWH = getWeightHandle(ctx, I->getScale());
  auto varH = getWeightHandle(ctx, I->getVar());
  auto meanH = getWeightHandle(ctx, I->getMean());

  auto channelIdx = I->getChannelIdx();
  auto epsilon = I->getEpsilon();

  // http://cthorey.github.io./backpropagation/
  //
  // mu = 1/N*np.sum(h,axis =0)
  // sigma2 = 1/N*np.sum((h-mu)**2)
  // hath = (h-mu)*(sigma2+epsilon)**(-1./2.)
  // y = gamma*hath+beta

  // In inference mode just apply the transformation:
  // y[i] = (x - mu) * gamma / stdvar + beta;
  for (size_t i = 0, e = inW.size(); i < e; i++) {
    size_t channelId = inW.getDimForPtr(channelIdx, i);
    FloatTy x = inW.raw(i);

    FloatTy mu = meanH.at({channelId});
    FloatTy var = varH.at({channelId});

    FloatTy stdvar = FloatTy(1.0) / std::sqrt(var + epsilon);

    FloatTy gamma = gammaWH.at({channelId});
    FloatTy beta = betaWH.at({channelId});

    outW.raw(i) = (x - mu) * gamma * stdvar + beta;
  }
}

void Interpreter::fwdBatchNormalizationInst_train(
    Context *ctx, const BatchNormalizationInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto varH = getWeightHandle(ctx, I->getVar());
  auto meanH = getWeightHandle(ctx, I->getMean());

  auto channelIdx = I->getChannelIdx();
  auto momentum = I->getMomentum();

  Tensor localMean(ElemKind::FloatTy, meanH.dims());
  Tensor localVar(ElemKind::FloatTy, varH.dims());
  auto localMeanH = localMean.getHandle<FloatTy>();
  auto localVarH = localVar.getHandle<FloatTy>();

  // The number of different channels.
  const size_t numChannels = inW.dims()[channelIdx];
  // THe number of elements that each channel holds.
  const size_t samplesPerChannel = inW.size() / numChannels;

  // Calculate Mean:

  // sum(in[i])
  for (size_t i = 0, e = inW.size(); i < e; i++) {
    size_t channelId = inW.getDimForPtr(channelIdx, i);
    FloatTy v = inW.raw(i);
    localMeanH.raw(channelId) += v;
  }
  // Mean = sum(in[i]) / N
  for (size_t i = 0, e = localMeanH.size(); i < e; i++) {
    localMeanH.at({i}) /= samplesPerChannel;
  }

  // Calculate Variance:

  // sum((x - mu) ^ 2)
  for (size_t i = 0, e = inW.size(); i < e; i++) {
    size_t channelId = inW.getDimForPtr(channelIdx, i);
    FloatTy v = inW.raw(i) - localMeanH.at({channelId});
    localVarH.raw(channelId) += v * v;
  }
  // Var = sum((x - mu) ^ 2) / N
  for (size_t i = 0, e = localMeanH.size(); i < e; i++) {
    localVarH.at({i}) /= samplesPerChannel;
  }

  // Update the global variance and mean:
  for (size_t i = 0, e = localMeanH.size(); i < e; i++) {
    auto P = momentum;
    meanH.at({i}) = P * localMeanH.at({i}) + (1 - P) * meanH.at({i});
    varH.at({i}) = P * localVarH.at({i}) + (1 - P) * varH.at({i});
  }

  // TODO: should we be using the running mean or the local mean?
  fwdBatchNormalizationInst_infer(ctx, I);
}

void Interpreter::bwdBatchNormalizationInst(Context *ctx,
                                            const BatchNormalizationInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outG = getGradHandle(ctx, I->getDest());

  auto gammaWH = getWeightHandle(ctx, I->getScale());
  auto betaGH = getGradHandle(ctx, I->getBias());
  auto gammaGH = getGradHandle(ctx, I->getScale());

  auto varH = getWeightHandle(ctx, I->getVar());
  auto meanH = getWeightHandle(ctx, I->getMean());

  auto channelIdx = I->getChannelIdx();
  auto epsilon = I->getEpsilon();

  // Update the gradient of the incoming buffer:
  Tensor dyhmu(ElemKind::FloatTy, meanH.dims());
  Tensor sumDy(ElemKind::FloatTy, meanH.dims());
  auto dyhmuH = dyhmu.getHandle<FloatTy>();
  auto sumDyH = sumDy.getHandle<FloatTy>();

  // The number of different channels.
  const size_t numChannels = inW.dims()[channelIdx];
  // THe number of elements that each channel holds.
  const size_t samplesPerChannel = inW.size() / numChannels;

  // Calculate: sum(dy * (h - mu))
  for (size_t i = 0, e = inW.size(); i < e; i++) {
    size_t channelId = inW.getDimForPtr(channelIdx, i);
    // x - mean.
    FloatTy cx = inW.raw(i) - meanH.at({channelId});
    // dy * (h - mu)
    dyhmuH.at({channelId}) += outG.raw(i) * cx;
  }

  // Calculate: sum(dy)
  for (size_t i = 0, e = inW.size(); i < e; i++) {
    size_t channelId = inW.getDimForPtr(channelIdx, i);
    sumDyH.at({channelId}) += outG.raw(i);
  }

  // http://cthorey.github.io./backpropagation/
  //
  // mu = 1./N*np.sum(h)
  // var = 1./N*np.sum((h-mu)**2)
  // dbeta = np.sum(dy)
  // dgamma = np.sum((h - mu) * (var + eps)**(-1. / 2.) * dy)
  // dh = (1. / N) * gamma * (var + eps)**(-1. / 2.) *
  //     (N * dy - np.sum(dy) - (h - mu) * 1/(var + eps) *
  //     np.sum(dy * (h - mu)))
  //
  for (size_t i = 0, e = inW.size(); i < e; i++) {
    size_t channelId = inW.getDimForPtr(channelIdx, i);

    FloatTy invN = (FloatTy(1.0) / samplesPerChannel);
    FloatTy gamma = gammaWH.at({channelId});
    FloatTy var = varH.at({channelId});
    FloatTy mu = meanH.at({channelId});
    FloatTy invVarSqrt = FloatTy(1.0) / std::sqrt(var + epsilon);
    FloatTy invVar = FloatTy(1.0) / (var + epsilon);

    FloatTy dy = outG.raw(i);
    FloatTy hmu = inW.raw(i) - mu;
    FloatTy sdy = sumDyH.at(channelId);
    FloatTy sdyhmu = dyhmuH.at(channelId);
    inG.raw(i) += invN * gamma * invVarSqrt *
                  (samplesPerChannel * dy - sdy - hmu * invVar * sdyhmu);
  }

  // Update the gradient of beta and gamma.
  for (size_t i = 0, e = inW.size(); i < e; i++) {
    size_t channelId = inW.getDimForPtr(channelIdx, i);

    FloatTy mu = meanH.at({channelId});
    FloatTy var = varH.at({channelId});
    FloatTy invVarSqrt = FloatTy(1.0) / std::sqrt(var + epsilon);

    betaGH.at({channelId}) += outG.raw(i);
    gammaGH.at({channelId}) += (inW.raw(i) - mu) * invVarSqrt * outG.raw(i);
  }
}

void Interpreter::fwdLocalResponseNormalizationInst(
    glow::Context *ctx, bool isTrain,
    const glow::LocalResponseNormalizationInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto scaleCache = getWeightHandle(ctx, I->getScale());

  ShapeNHWC odim(outW.dims());
  ShapeNHWC idim(inW.dims());

  (void)odim;

  // LRN node does not change the shape of the input.
  assert(odim == idim && "Output of LRN node must be same shape as input");

  // LRN node normalizes across channels, so the input must have a minimum
  // depth of 1.
  assert(idim.c > 0 && "Input of LRN node must have a minimum depth of 1");

  auto halfWindowSize = I->gethalfWindowSize();
  auto k = I->getK();
  auto beta = I->getBeta();
  auto windowSize = 2 * halfWindowSize + 1;
  auto normedAlpha = I->getAlpha() / windowSize;

  // For every input in the batch:
  for (size_t n = 0; n < idim.n; n++) {

    // For every row:
    for (size_t h = 0; h < idim.h; h++) {

      // For every column:
      for (size_t w = 0; w < idim.w; w++) {

        FloatTy squareSum = 0.0;

        // Compute squareSum for first channel.
        for (size_t c = 1; c <= halfWindowSize && c < idim.c; c++) {
          auto val = inW.at({n, h, w, c});
          squareSum += (val * val);
        }

        // For every channel:
        for (size_t c = 0; c < idim.c; c++) {
          auto scale = k + normedAlpha * squareSum;

          // This will be used to accelerate the backward pass.
          scaleCache.at({n, h, w, c}) = scale;

          auto normFactor = std::pow(scale, -beta);
          outW.at({n, h, w, c}) = inW.at({n, h, w, c}) * normFactor;

          // Modify squareSum for next channel.
          auto subIndex = c - halfWindowSize;
          auto addIndex = c + halfWindowSize + 1;
          auto sub = (c >= halfWindowSize) ? inW.at({n, h, w, subIndex}) : 0;
          auto add = (addIndex < idim.c) ? inW.at({n, h, w, addIndex}) : 0;

          // Subtract out "rear" end of this window, add "front" end of next.
          squareSum = squareSum - (sub * sub) + (add * add);
        }
      }
    }
  }
}

void Interpreter::bwdLocalResponseNormalizationInst(
    glow::Context *ctx, const glow::LocalResponseNormalizationInst *I) {
  auto inW = getWeightHandle(ctx, I->getSrc());
  auto inG = getGradHandle(ctx, I->getSrc());
  auto outW = getWeightHandle(ctx, I->getDest());
  auto outG = getGradHandle(ctx, I->getDest());
  auto scaleCache = getWeightHandle(ctx, I->getScale());

  ShapeNHWC odim(outW.dims());

  auto halfWindowSize = I->gethalfWindowSize();
  auto beta = I->getBeta();
  auto windowSize = 2 * halfWindowSize + 1;
  auto normedAlpha = I->getAlpha() / windowSize;

  // For every input in the batch:
  for (size_t n = 0; n < odim.n; n++) {

    // For every row:
    for (size_t h = 0; h < odim.h; h++) {

      // For every column:
      for (size_t w = 0; w < odim.w; w++) {

        FloatTy sum = 0.0;

        // Compute sum for first channel.
        for (size_t c = 1; c <= halfWindowSize && c < odim.c; c++) {
          auto outw = outW.at({n, h, w, c});
          auto scale = scaleCache.at({n, h, w, c});
          auto outg = outG.at({n, h, w, c});
          sum += (outg * (outw / scale));
        }

        // For every channel:
        for (size_t c = 0; c < odim.c; c++) {
          auto outg = outG.at({n, h, w, c});
          auto scale = scaleCache.at({n, h, w, c});
          auto inw = inW.at({n, h, w, c});

          inG.at({n, h, w, c}) = outg * std::pow(scale, -beta) -
                                 2 * normedAlpha * beta * inw * sum;

          // Modify sum for next channel.
          auto subIndex = c - halfWindowSize;
          auto addIndex = c + halfWindowSize + 1;

          if (c >= halfWindowSize) {
            auto outw = outW.at({n, h, w, subIndex});
            auto scale = scaleCache.at({n, h, w, subIndex});
            auto outg = outG.at({n, h, w, subIndex});

            // Subtract "rear" end of this window.
            sum -= (outg * (outw / scale));
          }

          if (addIndex < odim.c) {
            auto outw = outW.at({n, h, w, addIndex});
            auto scale = scaleCache.at({n, h, w, addIndex});
            auto outg = outG.at({n, h, w, addIndex});

            // Add "front" end of next window.
            sum += (outg * (outw / scale));
          }
        }
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                       Arithmetic operations
//===----------------------------------------------------------------------===//

void Interpreter::fwdArithmeticInst(Context *ctx, bool isTrain,
                                    const ArithmeticInst *I) {
  auto outW = getWeightHandle(ctx, I->getDest());
  auto LHSW = getWeightHandle(ctx, I->getLHS());
  auto RHSW = getWeightHandle(ctx, I->getRHS());

  switch (I->getKind()) {
  case ArithmeticInst::OpKind::Add:
    for (size_t i = 0, e = outW.size(); i < e; i++) {
      outW.raw(i) = LHSW.raw(i) + RHSW.raw(i);
    }
    return;
    break;

  case ArithmeticInst::OpKind::Mul:
    for (size_t i = 0, e = outW.size(); i < e; i++) {
      outW.raw(i) = LHSW.raw(i) * RHSW.raw(i);
    }
    return;
    break;
  }
}

void Interpreter::bwdArithmeticInst(Context *ctx, const ArithmeticInst *I) {
  auto LHSW = getWeightHandle(ctx, I->getLHS());
  auto RHSW = getWeightHandle(ctx, I->getRHS());
  auto outG = getGradHandle(ctx, I->getDest());
  auto LHSG = getGradHandle(ctx, I->getLHS());
  auto RHSG = getGradHandle(ctx, I->getRHS());

  switch (I->getKind()) {
  case ArithmeticInst::OpKind::Add:
    for (size_t i = 0, e = outG.size(); i < e; i++) {
      LHSG.raw(i) = outG.raw(i);
      RHSG.raw(i) = outG.raw(i);
    }
    return;
    break;

  case ArithmeticInst::OpKind::Mul:
    for (size_t i = 0, e = outG.size(); i < e; i++) {
      LHSG.raw(i) = RHSW.raw(i) * outG.raw(i);
      RHSG.raw(i) = LHSW.raw(i) * outG.raw(i);
    }
    return;
    break;
  }
}

//===----------------------------------------------------------------------===//
//                  Tensor allocation operations
//===----------------------------------------------------------------------===//

void Interpreter::fwdAllocActivationInst(Context *ctx, bool isTrain,
                                         const AllocActivationInst *I) {
  allocateBackingTensor(I);
  // Prepare for the next backprop iteration by zeroing the gradient
  // tensors. Notice that this only zeros the temporary grad tensors that
  // match the output tensors but not the gradient tensors that are
  // paired with filters. These are cleared during the learning process
  // at the end of the batch.
  getOrCreateGradTensor(I)->zero();
}

void Interpreter::bwdAllocActivationInst(Context *ctx,
                                         const AllocActivationInst *I) {}

void Interpreter::fwdDeallocActivationInst(Context *ctx, bool isTrain,
                                           const DeallocActivationInst *I) {}

void Interpreter::bwdDeallocActivationInst(Context *ctx,
                                           const DeallocActivationInst *I) {}