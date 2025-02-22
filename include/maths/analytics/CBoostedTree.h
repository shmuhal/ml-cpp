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

#ifndef INCLUDED_ml_maths_analytics_CBoostedTree_h
#define INCLUDED_ml_maths_analytics_CBoostedTree_h

#include <core/CDataFrame.h>
#include <core/CStatePersistInserter.h>
#include <core/CStateRestoreTraverser.h>

#include <maths/analytics/CBoostedTreeHyperparameters.h>
#include <maths/analytics/CBoostedTreeUtils.h>
#include <maths/analytics/CDataFrameCategoryEncoder.h>
#include <maths/analytics/CDataFramePredictiveModel.h>
#include <maths/analytics/ImportExport.h>

#include <maths/common/CLinearAlgebraEigen.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ml {
namespace core {
class CPackedBitVector;
}
namespace maths {
namespace analytics {
namespace boosted_tree {
class CLoss;
}
class CDataFrameCategoryEncoder;
class CEncodedDataFrameRowRef;
class CBoostedTreeImpl;

//! \brief A node of a regression tree.
//!
//! DESCRIPTION:\n
//! This defines a tree structure on a vector of nodes (maintaining the parent
//! child relationships as indexes into the vector). It holds the (binary)
//! splitting criterion (feature and value) and the tree's prediction at each
//! leaf. The intervals are open above so the left node contains feature vectors
//! for which the feature value is _strictly_ less than the split value.
//!
//! During training row masks are maintained for each node (so the data can be
//! efficiently traversed). This supports extracting the left and right child
//! node bit masks from the node's bit mask.
class MATHS_ANALYTICS_EXPORT CBoostedTreeNode final {
public:
    using TFloatVec = std::vector<common::CFloatStorage>;
    using TFloatVecVec = std::vector<TFloatVec>;
    using TSizeVec = std::vector<std::size_t>;
    using TNodeIndex = std::uint32_t;
    using TNodeIndexNodeIndexPr = std::pair<TNodeIndex, TNodeIndex>;
    using TPackedBitVectorPackedBitVectorPr =
        std::pair<core::CPackedBitVector, core::CPackedBitVector>;
    using TNodeVec = std::vector<CBoostedTreeNode>;
    using TOptionalNodeIndex = std::optional<TNodeIndex>;
    using TVector = common::CDenseVector<double>;
    using TRowRef = core::CDataFrame::TRowRef;

    class MATHS_ANALYTICS_EXPORT CVisitor {
    public:
        virtual ~CVisitor() noexcept = default;
        //! Adds to last added tree.
        virtual void addNode(std::size_t splitFeature,
                             double splitValue,
                             bool assignMissingToLeft,
                             const TVector& nodeValue,
                             double gain,
                             std::size_t numberSamples,
                             TOptionalNodeIndex leftChild,
                             TOptionalNodeIndex rightChild) = 0;
    };

public:
    CBoostedTreeNode() = default;
    explicit CBoostedTreeNode(std::size_t numberLossParameters);

    //! Check if this is a leaf node.
    bool isLeaf() const { return m_LeftChild == std::nullopt; }

    //! Get the leaf index for \p row.
    TNodeIndex leafIndex(const CEncodedDataFrameRowRef& row,
                         const TNodeVec& tree,
                         TNodeIndex index = 0) const;

    //! Get the value predicted by \p tree for the feature vector \p row.
    const TVector& value(const CEncodedDataFrameRowRef& row, const TNodeVec& tree) const {
        return tree[this->leafIndex(row, tree)].m_NodeValue;
    }

    //! Check if we should assign \p row to the left leaf.
    bool assignToLeft(const CEncodedDataFrameRowRef& row) const;

    //! \name Optimized for Train
    //!
    //! \note This provides overloads which are optimised for train. They can
    //! only be used when the splits used to generate the node are cached in
    //! the row and should be handled with care.
    //@{
    //! Get the leaf index for \p row.
    TNodeIndex leafIndex(const TRowRef& row,
                         const TSizeVec& extraColumns,
                         const TNodeVec& tree,
                         TNodeIndex index = 0) const;

    //! Get the value predicted by \p tree for the feature vector \p row.
    const TVector&
    value(const TRowRef& row, const TSizeVec& extraColumns, const TNodeVec& tree) const {
        return tree[this->leafIndex(row, extraColumns, tree)].m_NodeValue;
    }

    //! Check if we should assign \p row to the left leaf.
    bool assignToLeft(const TRowRef& row, const TSizeVec& extraColumns) const;
    //@}

    //! Get the value of this node.
    const TVector& value() const { return m_NodeValue; }

    //! Set the node value to \p value.
    void value(TVector value) { m_NodeValue = std::move(value); }

    //! Get the gain of the split.
    double gain() const { return m_Gain; }

    //! Get the gain variance for alternative splits.
    double gainVariance() const { return m_GainVariance; }

    //! Get the total curvature at the rows below this node.
    double curvature() const { return m_Curvature; }

    //! Set the number of samples to \p value.
    void numberSamples(std::size_t value);

    //! Get number of samples affected by the node.
    std::size_t numberSamples() const;

    //! Get the index of the left child node.
    TNodeIndex leftChildIndex() const { return *m_LeftChild; }

    //! Set the left child index to \p value.
    void leftChildIndex(TNodeIndex value) { m_LeftChild = value; }

    //! Get the index of the right child node.
    TNodeIndex rightChildIndex() const { return *m_RightChild; }

    //! Set the right child index to \p value.
    void rightChildIndex(TNodeIndex value) { m_RightChild = value; }

    //! Split this node and add its child nodes to \p tree.
    TNodeIndexNodeIndexPr split(const TFloatVecVec& candidateSplits,
                                std::size_t splitFeature,
                                double splitValue,
                                bool assignMissingToLeft,
                                double gain,
                                double gainVariance,
                                double curvature,
                                TNodeVec& tree);

    //! Split this node and add its child nodes to \p tree.
    //!
    //! \note This overload stubs out \p candidateSplits. This is used to optimize
    //! leaf access and assignToLeft for train so it is fine to supply empty.
    TNodeIndexNodeIndexPr split(std::size_t splitFeature,
                                double splitValue,
                                bool assignMissingToLeft,
                                double gain,
                                double gainVariance,
                                double curvature,
                                TNodeVec& tree) {
        return this->split({}, splitFeature, splitValue, assignMissingToLeft,
                           gain, gainVariance, curvature, tree);
    }

    //! Get the feature index of the split.
    std::size_t splitFeature() const { return m_SplitFeature; }

    //! Get the feature value at which to split .
    double splitValue() const { return m_SplitValue; }

    //! Get the memory used by this object.
    std::size_t memoryUsage() const;

    //! Get the node's memory usage for a loss function with \p numberLossParameters
    //! parameters.
    static std::size_t estimateMemoryUsage(std::size_t numberLossParameters);

    //! Get the memory the node will will use in bytes when deployed.
    std::size_t deployedSize() const;

    //! Persist by passing information to \p inserter.
    void acceptPersistInserter(core::CStatePersistInserter& inserter) const;

    //! Populate the object from serialized data.
    bool acceptRestoreTraverser(core::CStateRestoreTraverser& traverser);

    //! Visit this node.
    void accept(CVisitor& visitor) const;

    //! Get a human readable description of this tree.
    std::string print(const TNodeVec& tree) const;

private:
    std::ostringstream&
    doPrint(std::string pad, const TNodeVec& tree, std::ostringstream& result) const;

private:
    std::size_t m_SplitFeature{0};
    double m_SplitValue{0.0};
    std::uint8_t m_Split{0};
    std::uint8_t m_MissingSplit{0};
    bool m_AssignMissingToLeft{true};
    TOptionalNodeIndex m_LeftChild;
    TOptionalNodeIndex m_RightChild;
    TVector m_NodeValue;
    double m_Gain{0.0};
    double m_GainVariance{0.0};
    double m_Curvature{0.0};
    std::size_t m_NumberSamples{0};
};

//! \brief A boosted regression tree model.
//!
//! DESCRIPTION:\n
//! This is strongly based on xgboost. We deviate in some important respects: we have
//! hyperparameters which control the chance of selecting a feature in the feature
//! bag for a tree, we have automatic handling of categorical fields, we automatically
//! perform hyperparameter optimisation based on Bayesian Optimisation, we use soft
//! depth based regularisation and we can vary the learn rate while training a single
//! forest.
//!
//! The probability of selecting a feature behaves like a feature weight, allowing us
//! to:
//!   1. Incorporate an estimate of strength of relationship between a regressor and
//!      the target variable upfront,
//!   2. Use optimisation techniques suited for smooth cost functions to fine tune
//!      the regressors the tree will focus on during training.
//! All in all this gives us improved resilience to nuisance regressors and allows
//! us to perform feature selection by imposing a hard cutoff on the minimum probability
//! of a feature we will accept in the final model.
//!
//! The original xgboost paper doesn't explicitly deal with categorical data, it assumes
//! there is a well ordering on each feature and looks for binary splits subject to this
//! ordering. We use a mixed strategy which considers one-hot, target mean and frequency
//! encoding. We choose the "best" strategy based on simultaneously maximising measures
//! of relevancy and redundancy in the feature set as a whole. We use the MICe statistic
//! proposed by Reshef for this purpose. See CDataFrameCategoryEncoder for more details.
class MATHS_ANALYTICS_EXPORT CBoostedTree final : public CDataFramePredictiveModel {
public:
    using TStrVec = std::vector<std::string>;
    using TLossFunction = boosted_tree::CLoss;
    using TLossFunctionUPtr = std::unique_ptr<TLossFunction>;
    using TDataFramePtr = core::CDataFrame*;
    using TNodeVec = std::vector<CBoostedTreeNode>;
    using TNodeVecVec = std::vector<TNodeVec>;
    using THyperparameterImportanceVec =
        std::vector<boosted_tree_detail::SHyperparameterImportance>;

    class MATHS_ANALYTICS_EXPORT CVisitor : public CDataFrameCategoryEncoder::CVisitor,
                                            public CBoostedTreeNode::CVisitor {
    public:
        virtual void addTree() = 0;
        virtual void addClassificationWeights(TDoubleVec weights) = 0;
        virtual void addLossFunction(const TLossFunction& lossFunction) = 0;
    };

public:
    ~CBoostedTree() override;

    CBoostedTree(const CBoostedTree&) = delete;
    CBoostedTree& operator=(const CBoostedTree&) = delete;

    //! Train on the examples in the data frame supplied to the constructor.
    void train() override;

    //! Incrementally train the current model.
    //!
    //! \warning Train must have been previously called or a model loaded.
    void trainIncremental() override;

    //! Write the predictions to the data frame supplied to the constructor.
    //!
    //! \param[in] newDataOnly Only predict newly supplied data.
    //! \warning This can only be called after train.
    void predict(bool newDataOnly = false) const override;

    //! Get the SHAP value calculator.
    //!
    //! \warning Will return a nullptr if a trained model isn't available.
    CTreeShapFeatureImportance* shap() const override;

    //! Get the vector of hyperparameter importances.
    THyperparameterImportanceVec hyperparameterImportance() const;

    //! Get the number of rows used to train the model.
    std::size_t numberTrainRows() const override;

    //! Get the mean gap in the loss between test and train examples.
    double lossGap() const override;

    //! Get the column containing the dependent variable.
    std::size_t columnHoldingDependentVariable() const override;

    //! Get a mask for the new training data.
    const core::CPackedBitVector& newTrainingRowMask() const override;

    //! Read the model prediction from \p row.
    TDouble2Vec prediction(const TRowRef& row) const override;

    //! Read the previous model prediction from \p row if it has been updated.
    TDouble2Vec previousPrediction(const TRowRef& row) const override;

    //! Read the raw model prediction from \p row and make posthoc adjustments.
    //!
    //! For example, classification multiplicative weights are used for each
    //! class to target different objectives (accuracy or minimum recall) when
    //! assigning classes.
    TDouble2Vec adjustedPrediction(const TRowRef& row) const override;

    //! Get the selected rows that summarize.
    core::CPackedBitVector dataSummarization() const override;

    //! Get the category encoder.
    const CDataFrameCategoryEncoder& categoryEncoder() const override;

    //! Get the model produced by training if it has been run.
    const TNodeVecVec& trainedModel() const;

    //! Persist by passing information to \p inserter.
    void acceptPersistInserter(core::CStatePersistInserter& inserter) const;

    //! Populate the object from serialized data.
    bool acceptRestoreTraverser(core::CStateRestoreTraverser& traverser);

    //! Visit this tree trainer.
    void accept(CVisitor& visitor) const;

    //! \name Test Only
    //@{
    //! Get the implementation.
    CBoostedTreeImpl& impl() const;

    //! Get the weight that has been chosen for each feature for training.
    const TDoubleVec& featureWeightsForTraining() const;

    //! \return The hyperparameters.
    const CBoostedTreeHyperparameters& hyperparameters() const;

    //! \return The classification weights vector.
    TDoubleVec classificationWeights() const;
    //@}

private:
    using TImplUPtr = std::unique_ptr<CBoostedTreeImpl>;

private:
    CBoostedTree(core::CDataFrame& frame,
                 TTrainingStateCallback recordTrainingState,
                 TImplUPtr&& impl) noexcept;

private:
    TImplUPtr m_Impl;

    friend class CBoostedTreeFactory;
};
}
}
}

#endif // INCLUDED_ml_maths_analytics_CBoostedTree_h
