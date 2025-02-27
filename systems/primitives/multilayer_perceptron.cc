#include "drake/systems/primitives/multilayer_perceptron.h"

#include "drake/common/default_scalars.h"
#include "drake/systems/framework/basic_vector.h"

namespace drake {
namespace systems {

using Eigen::MatrixXd;
using Eigen::VectorXd;

namespace {

std::vector<PerceptronActivationType> MakeDefaultActivations(
    int num, PerceptronActivationType activation_type) {
  std::vector<PerceptronActivationType> types(num, activation_type);
  types[num - 1] = kIdentity;
  return types;
}

}  // namespace

template <typename T>
MultilayerPerceptron<T>::MultilayerPerceptron(
    const std::vector<int>& layers, PerceptronActivationType activation_type)
    : MultilayerPerceptron<T>(
          layers, MakeDefaultActivations(layers.size() - 1, activation_type)) {}

template <typename T>
MultilayerPerceptron<T>::MultilayerPerceptron(
    const std::vector<int>& layers,
    const std::vector<PerceptronActivationType>& activation_types)
    : LeafSystem<T>(SystemTypeTag<MultilayerPerceptron>{}),
      num_hidden_layers_(layers.size() - 2),
      num_weights_(layers.size() - 1),
      layers_(layers),
      activation_types_(activation_types) {
  DRAKE_DEMAND(num_hidden_layers_ >= 1);  // Otherwise its not "multilayer"!
  DRAKE_DEMAND(activation_types_.size() == layers.size() - 1);
  for (int units_in_layer : layers) {
    DRAKE_DEMAND(units_in_layer > 0);
  }
  this->DeclareVectorInputPort("x", layers[0]);
  this->DeclareVectorOutputPort("y", layers[num_weights_],
                                &MultilayerPerceptron<T>::CalcOutput);

  num_parameters_ = 0;
  weight_indices_.reserve(num_weights_);
  bias_indices_.reserve(num_weights_);
  for (int i = 0; i < num_weights_; ++i) {
    weight_indices_[i] = num_parameters_;
    num_parameters_ += layers[i + 1] * layers[i];
    bias_indices_[i] = num_parameters_;
    num_parameters_ += layers[i + 1];

    if (activation_types_[i] == kTanh) {
      sigma_.push_back([](const Eigen::Ref<const MatrixX<T>>& X) {
        return X.array().tanh().matrix();
      });
      dsigma_.push_back([](const Eigen::Ref<const MatrixX<T>>& X) {
        return (1.0 - X.array().tanh().square()).matrix();
      });
    } else if (activation_types_[i] == kReLU) {
      sigma_.push_back([](const Eigen::Ref<const MatrixX<T>>& X) {
        return X.array().max(0.0).matrix();
      });
      dsigma_.push_back([](const Eigen::Ref<const MatrixX<T>>& X) {
        return (X.array() <= 0).select(MatrixX<T>::Zero(X.rows(), X.cols()), 1);
      });
    } else {
      DRAKE_DEMAND(activation_types_[i] == kIdentity);
      sigma_.push_back([](const Eigen::Ref<const MatrixX<T>>& X) { return X; });
      dsigma_.push_back([](const Eigen::Ref<const MatrixX<T>>& X) {
        return MatrixX<T>::Ones(X.rows(), X.cols());
      });
    }
  }
  this->DeclareNumericParameter(
      BasicVector<T>(VectorX<T>::Zero(num_parameters_)));

  // Declare cache entry for CalcOutput.
  std::vector<VectorX<T>> hidden_layers;
  for (int i = 0; i < num_hidden_layers_; ++i) {
    hidden_layers.push_back(VectorX<T>::Zero(layers[i + 1]));
  }
  hidden_layer_cache_ =
      &this->DeclareCacheEntry("hidden_layer", hidden_layers,
                               &MultilayerPerceptron<T>::CalcHiddenLayers);

  // Declare cache entries for Backpropagation:
  std::vector<MatrixX<T>> backprop_data(5 * num_weights_);
  backprop_cache_ = &this->DeclareCacheEntry(
      "backprop", backprop_data, &MultilayerPerceptron<T>::BackpropCacheNoOp);
  // Wx_plus_b is num_weights_ matrices starting at std::vector index 0.
  Wx_plus_b_cache_index_ = 0;
  // The Xn[0] matrix is at backprop_cache_[num_weights], etc.
  Xn_cache_index_ = num_weights_;
  dloss_dXn_cache_index_ = 2 * num_weights_;
  dloss_dWx_plus_b_cache_index_ = 3 * num_weights_;
  dloss_dW_cache_index_ = 4 * num_weights_;
}

template <typename T>
template <typename U>
MultilayerPerceptron<T>::MultilayerPerceptron(
    const MultilayerPerceptron<U>& other)
    : MultilayerPerceptron<T>(other.layers(), other.activation_types_) {}

template <typename T>
const VectorX<T>& MultilayerPerceptron<T>::GetParameters(
    const Context<T>& context) const {
  return context.get_numeric_parameter(0).value();
}

template <typename T>
void MultilayerPerceptron<T>::SetRandomParameters(
    const Context<T>& context, Parameters<T>* parameters,
    RandomGenerator* generator) const {
  // TODO(russt): Consider more advanced approaches, e.g. Xavier initialization.
  unused(context);
  std::normal_distribution<double> normal(0.0, 0.01);
  BasicVector<T>& params = parameters->get_mutable_numeric_parameter(0);
  for (int i = 0; i < num_parameters_; ++i) {
    params[i] = normal(*generator);
  }
}

template <typename T>
void MultilayerPerceptron<T>::SetParameters(
    Context<T>* context, const Eigen::Ref<const VectorX<T>>& params) const {
  DRAKE_DEMAND(params.rows() == num_parameters_);
  context->get_mutable_numeric_parameter(0).SetFromVector(params);
}

template <typename T>
Eigen::Map<const MatrixX<T>> MultilayerPerceptron<T>::GetWeights(
    const Context<T>& context, int layer) const {
  return GetWeights(context.get_numeric_parameter(0).value(), layer);
}

template <typename T>
Eigen::Map<const VectorX<T>> MultilayerPerceptron<T>::GetBiases(
    const Context<T>& context, int layer) const {
  return GetBiases(context.get_numeric_parameter(0).value(), layer);
}

template <typename T>
void MultilayerPerceptron<T>::SetWeights(
    Context<T>* context, int layer,
    const Eigen::Ref<const MatrixX<T>>& W) const {
  DRAKE_DEMAND(layer >= 0 && layer < num_weights_);
  DRAKE_DEMAND(W.rows() == layers_[layer + 1]);
  DRAKE_DEMAND(W.cols() == layers_[layer]);
  BasicVector<T>& params = context->get_mutable_numeric_parameter(0);
  Eigen::Map<MatrixX<T>>(
      params.get_mutable_value().data() + weight_indices_[layer],
      layers_[layer + 1], layers_[layer]) = W;
}

template <typename T>
void MultilayerPerceptron<T>::SetBiases(
    Context<T>* context, int layer,
    const Eigen::Ref<const VectorX<T>>& b) const {
  DRAKE_DEMAND(layer >= 0 && layer < num_weights_);
  DRAKE_DEMAND(b.rows() == layers_[layer + 1]);
  context->get_mutable_numeric_parameter(0).get_mutable_value().segment(
      bias_indices_[layer], layers_[layer + 1]) = b;
}

template <typename T>
Eigen::Map<const MatrixX<T>> MultilayerPerceptron<T>::GetWeights(
    const Eigen::Ref<const VectorX<T>>& params, int layer) const {
  DRAKE_DEMAND(layer >= 0 && layer < num_weights_);
  DRAKE_DEMAND(params.rows() == num_parameters_);
  return Eigen::Map<const MatrixX<T>>(params.data() + weight_indices_[layer],
                                      layers_[layer + 1], layers_[layer]);
}

template <typename T>
Eigen::Map<const VectorX<T>> MultilayerPerceptron<T>::GetBiases(
    const Eigen::Ref<const VectorX<T>>& params, int layer) const {
  DRAKE_DEMAND(layer >= 0 && layer < num_weights_);
  DRAKE_DEMAND(params.rows() == num_parameters_);
  return Eigen::Map<const VectorX<T>>(params.data() + bias_indices_[layer],
                                      layers_[layer + 1]);
}

template <typename T>
void MultilayerPerceptron<T>::SetWeights(
    EigenPtr<VectorX<T>> params, int layer,
    const Eigen::Ref<const MatrixX<T>>& W) const {
  DRAKE_DEMAND(layer >= 0 && layer < num_weights_);
  DRAKE_DEMAND(params->rows() == num_parameters_);
  DRAKE_DEMAND(W.rows() == layers_[layer + 1]);
  DRAKE_DEMAND(W.cols() == layers_[layer]);
  Eigen::Map<MatrixX<T>>(params->data() + weight_indices_[layer],
                         layers_[layer + 1], layers_[layer]) = W;
}

template <typename T>
void MultilayerPerceptron<T>::SetBiases(
    EigenPtr<VectorX<T>> params, int layer,
    const Eigen::Ref<const VectorX<T>>& b) const {
  DRAKE_DEMAND(layer >= 0 && layer < num_weights_);
  DRAKE_DEMAND(params->rows() == num_parameters_);
  DRAKE_DEMAND(b.rows() == layers_[layer + 1]);
  params->segment(bias_indices_[layer], layers_[layer + 1]) = b;
}

template <typename T>
T MultilayerPerceptron<T>::Backpropagation(
    const Context<T>& context, const Eigen::Ref<const MatrixX<T>>& X,
    std::function<T(const Eigen::Ref<const MatrixX<T>>& Y,
                    EigenPtr<MatrixX<T>> dloss_dY)>
        loss,
    EigenPtr<VectorX<T>> dloss_dparams) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(X.rows() == layers_[0]);
  DRAKE_DEMAND(dloss_dparams->rows() == num_parameters_);
  // Note: Should aim for zero dynamic allocations in here (except on the
  // first calls and whenever X changes size).
  auto& cache = backprop_cache_->get_mutable_cache_entry_value(context)
                    .template GetMutableValueOrThrow<std::vector<MatrixX<T>>>();
  // TODO(russt): Try to reduce the amount of scratch memory.
  MatrixX<T>* Wx_plus_b = &cache[Wx_plus_b_cache_index_];
  MatrixX<T>* Xn = &cache[Xn_cache_index_];
  MatrixX<T>* dloss_dXn = &cache[dloss_dXn_cache_index_];
  MatrixX<T>* dloss_dWx_plus_b = &cache[dloss_dWx_plus_b_cache_index_];
  MatrixX<T>* dloss_dW = &cache[dloss_dW_cache_index_];
  // Forward pass:
  Wx_plus_b[0] = (GetWeights(context, 0) * X).colwise() + GetBiases(context, 0);
  Xn[0] = sigma_[0](Wx_plus_b[0]);
  for (int i = 1; i < num_weights_; ++i) {
    Wx_plus_b[i] =
        (GetWeights(context, i) * Xn[i - 1]).colwise() + GetBiases(context, i);
    Xn[i] = sigma_[i](Wx_plus_b[i]);
  }
  dloss_dXn[num_weights_ - 1].resize(layers_[num_weights_], X.cols());
  const T l = loss(Xn[num_weights_ - 1], &dloss_dXn[num_weights_ - 1]);
  // Backward pass:
  for (int i = num_weights_ - 1; i >= 0; --i) {
    dloss_dWx_plus_b[i] =
        (dloss_dXn[i].array() * dsigma_[i](Wx_plus_b[i]).array()).matrix();
    dloss_dW[i].resize(layers_[i + 1], layers_[i]);
    dloss_dW[i].setZero();
    for (int j = 0; j < X.cols(); ++j) {
      if (i > 0) {
        dloss_dW[i] +=
            dloss_dWx_plus_b[i].col(j) * Xn[i - 1].col(j).transpose();
      } else {
        dloss_dW[i] += dloss_dWx_plus_b[i].col(j) * X.col(j).transpose();
      }
    }
    SetWeights(dloss_dparams, i, dloss_dW[i]);
    SetBiases(dloss_dparams, i, dloss_dWx_plus_b[i].rowwise().sum());
    if (i > 0) {
      dloss_dXn[i - 1] =
          GetWeights(context, i).transpose() * dloss_dWx_plus_b[i];
    }
  }
  return l;
}

template <typename T>
T MultilayerPerceptron<T>::BackpropagationMeanSquaredError(
    const Context<T>& context, const Eigen::Ref<const MatrixX<T>>& X,
    const Eigen::Ref<const MatrixX<T>>& Y_desired,
    EigenPtr<VectorX<T>> dloss_dparams) const {
  DRAKE_DEMAND(Y_desired.rows() == layers_[num_weights_]);
  DRAKE_DEMAND(Y_desired.cols() == X.cols());
  // tests to cover the Backpropagation method.
  DRAKE_DEMAND(Y_desired.rows() == layers_[num_weights_]);
  DRAKE_DEMAND(Y_desired.cols() == X.cols());
  auto MSE_loss = [Y_desired](const Eigen::Ref<const MatrixX<T>>& Y,
                              EigenPtr<MatrixX<T>> dloss_dY) {
    *dloss_dY = 2.0 * (Y - Y_desired) / Y.cols();
    return (Y - Y_desired).squaredNorm() / Y.cols();
  };
  return Backpropagation(context, X, MSE_loss, dloss_dparams);
}

template <typename T>
void MultilayerPerceptron<T>::BatchOutput(
    const Context<T>& context, const Eigen::Ref<const MatrixX<T>>& X,
    EigenPtr<MatrixX<T>> Y) const {
  this->ValidateContext(context);
  DRAKE_DEMAND(X.rows() == layers_[0]);
  DRAKE_DEMAND(Y->rows() == layers_[num_weights_]);
  DRAKE_DEMAND(Y->cols() == X.cols());
  // Note: Should aim for zero dynamic allocations in here (except on the
  // first calls and whenever X changes size).
  auto& cache = backprop_cache_->get_mutable_cache_entry_value(context)
                    .template GetMutableValueOrThrow<std::vector<MatrixX<T>>>();
  // TODO(russt): Try to reduce the amount of scratch memory.
  MatrixX<T>* Wx_plus_b = &cache[Wx_plus_b_cache_index_];
  MatrixX<T>* Xn = &cache[Xn_cache_index_];
  // Forward pass:
  Wx_plus_b[0] = (GetWeights(context, 0) * X).colwise() + GetBiases(context, 0);
  Xn[0] = sigma_[0](Wx_plus_b[0]);
  for (int i = 1; i < num_weights_; ++i) {
    Wx_plus_b[i] =
        (GetWeights(context, i) * Xn[i - 1]).colwise() + GetBiases(context, i);
    if (i == num_weights_ - 1) {
      *Y = sigma_[i](Wx_plus_b[i]);
    } else {
      Xn[i] = sigma_[i](Wx_plus_b[i]);
    }
  }
}

template <typename T>
void MultilayerPerceptron<T>::CalcOutput(const Context<T>& context,
                                         BasicVector<T>* y) const {
  // TODO(russt): Test for and eliminate any dynamic allocations.
  this->ValidateContext(context);
  y->get_mutable_value() = sigma_[num_weights_ - 1](
      GetWeights(context, num_weights_ - 1) *
          hidden_layer_cache_->Eval<std::vector<VectorX<T>>>(
              context)[num_hidden_layers_ - 1] +
      GetBiases(context, num_weights_ - 1));
}

template <typename T>
void MultilayerPerceptron<T>::CalcHiddenLayers(
    const Context<T>& context, std::vector<VectorX<T>>* hidden) const {
  (*hidden)[0] =
      sigma_[0](GetWeights(context, 0) * this->get_input_port().Eval(context) +
                GetBiases(context, 0));
  for (int i = 1; i < num_hidden_layers_; ++i) {
    (*hidden)[i] = sigma_[i](GetWeights(context, i) * (*hidden)[i - 1] +
                             GetBiases(context, i));
  }
}

template <typename T>
void MultilayerPerceptron<T>::BackpropCacheNoOp(
    const Context<T>&, std::vector<MatrixX<T>>*) const {
  // Intentionally left blank. The values defining this computation are not in
  // the context, so we assign them directly in the backprop algorithm.
}

}  // namespace systems
}  // namespace drake

DRAKE_DEFINE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_SCALARS(
    class ::drake::systems::MultilayerPerceptron)
