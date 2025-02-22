/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License
 * 2.0 and the following additional limitation. Functionality enabled by the
 * files subject to the Elastic License 2.0 may only be used in production when
 * invoked by an Elasticsearch process with a license key installed that permits
 * use of machine learning features. You may not use this file except in
 * compliance with the Elastic License 2.0 and the foregoing additional
 * limitation.
 */

#include <maths/analytics/CBoostedTreeUtils.h>

#include <core/CContainerPrinter.h>
#include <core/Concurrency.h>

#include <maths/analytics/CBoostedTree.h>
#include <maths/analytics/CBoostedTreeHyperparameters.h>
#include <maths/analytics/CBoostedTreeLoss.h>
#include <maths/analytics/CDataFrameCategoryEncoder.h>

#include <maths/common/CLinearAlgebraEigen.h>
#include <maths/common/MathsTypes.h>

namespace ml {
namespace maths {
namespace analytics {
namespace boosted_tree_detail {
using namespace boosted_tree;

namespace {
using TDoubleVector = common::CDenseVector<double>;
using TDoubleVectorVec = std::vector<TDoubleVector>;
using TDoubleVectorVecVec = std::vector<TDoubleVectorVec>;

void propagateLossGradients(std::size_t node,
                            const std::vector<CBoostedTreeNode>& tree,
                            TDoubleVectorVec& lossGradients) {

    if (tree[node].isLeaf()) {
        return;
    }

    propagateLossGradients(tree[node].leftChildIndex(), tree, lossGradients);
    propagateLossGradients(tree[node].rightChildIndex(), tree, lossGradients);

    // A post order depth first traversal means that loss gradients are written
    // to child nodes before they are read here.
    lossGradients[node] += lossGradients[tree[node].leftChildIndex()];
    lossGradients[node] += lossGradients[tree[node].rightChildIndex()];
}
}

const CBoostedTreeNode& root(const std::vector<CBoostedTreeNode>& tree) {
    return tree[rootIndex()];
}

CBoostedTreeNode& root(std::vector<CBoostedTreeNode>& tree) {
    return tree[rootIndex()];
}

void zeroPrediction(const TRowRef& row, const TSizeVec& extraColumns, std::size_t numberLossParameters) {
    for (std::size_t i = 0; i < numberLossParameters; ++i) {
        row.writeColumn(extraColumns[E_Prediction] + i, 0.0);
    }
}

void writePrediction(const TRowRef& row,
                     const TSizeVec& extraColumns,
                     std::size_t numberLossParameters,
                     const TMemoryMappedFloatVector& value) {
    for (std::size_t i = 0; i < numberLossParameters; ++i) {
        row.writeColumn(extraColumns[E_Prediction] + i, value(i));
    }
}

void writePreviousPrediction(const TRowRef& row,
                             const TSizeVec& extraColumns,
                             std::size_t numberLossParameters,
                             const TMemoryMappedFloatVector& value) {
    for (std::size_t i = 0; i < numberLossParameters; ++i) {
        row.writeColumn(extraColumns[E_PreviousPrediction] + i, value(i));
    }
}

void zeroLossGradient(const TRowRef& row, const TSizeVec& extraColumns, std::size_t numberLossParameters) {
    for (std::size_t i = 0; i < numberLossParameters; ++i) {
        row.writeColumn(extraColumns[E_Gradient] + i, 0.0);
    }
}

void writeLossGradient(const TRowRef& row,
                       const CEncodedDataFrameRowRef& encodedRow,
                       bool newExample,
                       const TSizeVec& extraColumns,
                       const CLoss& loss,
                       const TMemoryMappedFloatVector& prediction,
                       double actual,
                       double weight) {
    auto writer = [&row, &extraColumns](std::size_t i, double value) {
        row.writeColumn(extraColumns[E_Gradient] + i, value);
    };
    // We wrap the writer in another lambda which we know takes advantage
    // of std::function small size optimization to avoid heap allocations.
    loss.gradient(encodedRow, newExample, prediction, actual,
                  [&writer](std::size_t i, double value) { writer(i, value); }, weight);
}

void zeroLossCurvature(const TRowRef& row, const TSizeVec& extraColumns, std::size_t numberLossParameters) {
    for (std::size_t i = 0, size = lossHessianUpperTriangleSize(numberLossParameters);
         i < size; ++i) {
        row.writeColumn(extraColumns[E_Curvature] + i, 0.0);
    }
}

void writeLossCurvature(const TRowRef& row,
                        const CEncodedDataFrameRowRef& encodedRow,
                        bool newExample,
                        const TSizeVec& extraColumns,
                        const CLoss& loss,
                        const TMemoryMappedFloatVector& prediction,
                        double actual,
                        double weight) {
    auto writer = [&row, &extraColumns](std::size_t i, double value) {
        row.writeColumn(extraColumns[E_Curvature] + i, value);
    };
    // We wrap the writer in another lambda which we know takes advantage
    // of std::function small size optimization to avoid heap allocations.
    loss.curvature(encodedRow, newExample, prediction, actual,
                   [&writer](std::size_t i, double value) { writer(i, value); }, weight);
}

TDoubleVec
retrainTreeSelectionProbabilities(std::size_t numberThreads,
                                  const core::CDataFrame& frame,
                                  const TSizeVec& extraColumns,
                                  std::size_t dependentVariable,
                                  const CDataFrameCategoryEncoder& encoder,
                                  const core::CPackedBitVector& trainingDataRowMask,
                                  const CLoss& loss,
                                  const std::vector<std::vector<CBoostedTreeNode>>& forest) {

    using TRowItr = core::CDataFrame::TRowItr;

    // The first tree is used to centre the data and we never retrain this.

    if (forest.size() < 2) {
        return {};
    }

    std::size_t numberLossParameters{loss.numberParameters()};
    LOG_TRACE(<< "dependent variable " << dependentVariable);
    LOG_TRACE(<< "number loss parameters = " << numberLossParameters);

    auto makeComputeTotalLossGradient = [&]() {
        return [&](TDoubleVectorVecVec& leafLossGradients,
                   const TRowItr& beginRows, const TRowItr& endRows) {
            for (auto row = beginRows; row != endRows; ++row) {
                auto prediction = readPrediction(*row, extraColumns, numberLossParameters);
                double actual{readActual(*row, dependentVariable)};
                double weight{readExampleWeight(*row, extraColumns)};
                for (std::size_t i = 0; i < forest.size(); ++i) {
                    auto encodedRow = encoder.encode(*row);
                    std::size_t leaf{root(forest[i]).leafIndex(encodedRow, forest[i])};
                    auto writer = [&](std::size_t j, double gradient) {
                        leafLossGradients[i][leaf](j) = gradient;
                    };
                    loss.gradient(encodedRow, true, prediction, actual, writer, weight);
                }
            }
        };
    };

    auto makeZeroLeafLossGradients = [&] {
        TDoubleVectorVecVec leafLossGradients;
        leafLossGradients.reserve(forest.size());
        for (const auto& tree : forest) {
            leafLossGradients.emplace_back(tree.size(), TDoubleVector::Zero(numberLossParameters));
        }
        return leafLossGradients;
    };

    auto computeLeafLossGradients = [&](const core::CPackedBitVector& rowMask) {
        auto results = frame.readRows(
            numberThreads, 0, frame.numberRows(),
            core::bindRetrievableState(makeComputeTotalLossGradient(),
                                       makeZeroLeafLossGradients()),
            &rowMask);

        TDoubleVectorVecVec leafLossGradients{std::move(results.first[0].s_FunctionState)};
        for (std::size_t i = 1; i < results.first.size(); ++i) {
            auto& resultsForWorker = results.first[i].s_FunctionState;
            for (std::size_t j = 0; j < resultsForWorker.size(); ++j) {
                for (std::size_t k = 0; k < resultsForWorker[j].size(); ++k) {
                    leafLossGradients[j][k] += resultsForWorker[j][k];
                }
            }
        }

        return leafLossGradients;
    };

    // Compute the sum of loss gradients for each node.
    auto trainingDataLossGradients = computeLeafLossGradients(trainingDataRowMask);
    for (std::size_t i = 0; i < forest.size(); ++i) {
        propagateLossGradients(rootIndex(), forest[i], trainingDataLossGradients[i]);
    }
    LOG_TRACE(<< "loss gradients = " << trainingDataLossGradients);

    // We are interested in choosing trees for which the total gradient of the
    // loss at the current predictions for all nodes is the largest. These trees,
    // at least locally, give the largest gain in loss by adjusting. Gradients
    // of internal nodes are defined as the sum of the leaf gradients below them.
    // We include these because they capture the fact that certain branches of
    // specific trees may be retrained for greater effect.

    TDoubleVec result(forest.size(), 0.0);
    double Z{0.0};
    for (std::size_t i = 1; i < forest.size(); ++i) {
        for (std::size_t j = 1; j < forest[i].size(); ++j) {
            result[i] += trainingDataLossGradients[i][j].lpNorm<1>();
        }
        Z += result[i];
    }
    for (auto& p : result) {
        p /= Z;
    }
    LOG_TRACE(<< "probabilities = " << result);

    return result;
}
}
}
}
}
