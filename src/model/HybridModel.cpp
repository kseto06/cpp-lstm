#include "HybridModel.h"
#include "MLP.h"
#include "LSTMNetwork.h"
#include "activations.h"

#include <cmath>
#include <vector>
#include <map>
#include <random>

#include "linalg.h"

namespace HybridModel {
    typedef std::vector<std::vector<double>> Matrix;
    typedef std::vector<std::vector<std::vector<double>>> Tensor3D;
    typedef std::variant<Matrix, Tensor3D> variantTensor;
    typedef std::map<std::string, Matrix> matrixDict; //Global params
    typedef std::vector<matrixDict> MLPCache; //cache for forward prop
    typedef std::tuple<Tensor3D, Matrix> minibatch;

    //LSTM
    typedef std::tuple<Matrix, Matrix, Matrix, Matrix, Matrix, Matrix, Matrix, Matrix, Matrix, matrixDict> cacheTuple;
    typedef std::tuple<Tensor3D, Tensor3D, Tensor3D, std::tuple<std::vector<cacheTuple>, Tensor3D>> LSTMCache;

    //Backprop
    //Variant since it can be either a Tensor3D gradient with timesteps or Matrix gradients
    typedef std::map<std::string, variantTensor> gradientDict;

    //Unified cache structure
    struct UnifiedCache {
        std::vector<std::variant<LSTMCache, matrixDict>> cache;
    };

    //Unified gradient structure
    struct UnifiedGradients {
        std::vector<std::variant<gradientDict, matrixDict>> grads;
    };

    //Anonymous namespace for private variables
    namespace {
        // Global model parameters
        std::vector<std::string> layer_types = {};
        std::vector<int> layer_dims = {};
        std::vector<matrixDict> layer_params;
        double learning_rate;

        //Forward prop variables
        UnifiedCache cache;
        Matrix finalPrediction; //Linear output matrix, shape(m,1)

        //Loss
        double accumulated_loss = 0.0;

        //Data, x_train and y_train. NOTE: x_train and y_train have to be generated by minibatches
        variantTensor x_train;
        Matrix y_train = {{}}; //shape (m,1)
        int BATCH_SIZE;
        int n_hidden; //Number of LSTM units.

        //Backprop variables
        UnifiedGradients grads;

        //Adam optimizer variables
        std::vector<std::vector<matrixDict>> Adam_params; //2D to store v and s
        int t = 0;
        const double beta1 = 0.9;
        const double beta2 = 0.999;
        const double epsilon = 1e-8;
    }

    // Minibatch generation
    std::vector<minibatch> generate_minibatches(const Tensor3D& X, const Matrix& Y, const int batch_size, const int seed) {
        //Training examples
        size_t m = X.size();

        // Generate permutations for each index
        std::vector<int> permutation(m);
        for (int i = 0; i < m; i++) {
            permutation[i] = i;
        }
        std::mt19937 perm(seed);
        std::shuffle(permutation.begin(), permutation.end(), perm);

        // Shuffle the dataset using the permutations
        Tensor3D shuffled_X(m, Matrix(X[0].size(), std::vector<double>(X[0][0].size())));
        Matrix shuffled_Y(m, std::vector<double>(1));
        for (size_t i = 0; i < m; i++) {
            shuffled_X[i] = X[permutation[i]];
            shuffled_Y[i] = Y[permutation[i]];
        }

        std::vector<minibatch> minibatches;
        for (size_t k = 0; k < m; k += batch_size) {
            int end = std::min(k + batch_size, m);

            // Correctly allocate batch size
            Tensor3D minibatch_X(end-k, Matrix(X[0].size(), std::vector<double>(X[0][0].size())));
            Matrix minibatch_Y(end-k, std::vector<double>(1));

            for (int i = k; i < end; i++) {
                minibatch_X[i - k] = shuffled_X[i];
                minibatch_Y[i - k] = shuffled_Y[i];
            }

            minibatches.emplace_back(std::move(minibatch_X), std::move(minibatch_Y));
        }

        return minibatches;
    }


    // MSE loss function
    double MSE(const std::vector<double>& pred, const std::vector<double>& target) {
        if (pred.size() != target.size()) {
            throw std::invalid_argument("Prediction and target sizes do not match");
        }

        double loss = 0.0;
        for (size_t i = 0; i < pred.size(); i++) {
            loss += std::pow(pred[i] - target[i], 2);
        }
        return loss/(2*pred.size());
    }

    //Inputting X and Y datasets:
    void init_data(const variantTensor& X, const Matrix& Y, const int batch_size) {
        x_train = X;
        y_train = Y;
        BATCH_SIZE = batch_size;
    }

    //Layer types and dimensions (setters)
    void init_layers(const std::vector<std::string>& layer_type, const std::vector<int>& layer_dim) {
        layer_types = layer_type;
        layer_dims = layer_dim;
    }

    // Initialization of the number of LSTM Cells/Units:
    void init_hidden_units(const int numUnits) {
        n_hidden = numUnits;
    }

    // Initialization of the learning rate
    void init_learning_rate(const double lr = 3e-4) {
        learning_rate = lr;
    }

    //LSTM/MLP Network initialization
    void initialize_network() {
        std::cout << "initialize_network - n_hidden: " << n_hidden << std::endl;
        //NOTE: layer_type and layer_dims should have the same shape
        for (int i = 1; i <= layer_types.size(); i++) {
            matrixDict current_params;
            std::cout << "Layer " << i << ": " << layer_types[i-1] << std::endl;

            if (layer_types[i-1] == "LSTM") {
                if (std::holds_alternative<Tensor3D>(x_train)) {
                    Tensor3D x = std::get<Tensor3D>(x_train);
                    int n_input = (i == 1) ? x[0][0].size() : layer_dims[i-2]; //Input features : output layers
                    current_params = LSTMNetwork::init_params(n_input, n_hidden, layer_dims[i-1], i);
                    std::cout << "LSTM init successful" << std::endl;
                } else {
                    std::cout << "Requires Tensor3D input for init" << std::endl;
                    linalg::printMatrix(std::get<Matrix>(x_train));
                }
            } else if (layer_types[i-1] == "Relu" || layer_types[i-1] == "Linear") {
                current_params = MLP::init_mlp_params(layer_dims, i-1);
                std::cout << "MLP init successful" << std::endl;
            }
            layer_params.push_back(current_params);
        }
    }

    //Tensor3D --> Matrix conversion based on last timestep output
    Matrix reshape_last_timestep(const Tensor3D& hidden_state) {
        int batch_size = hidden_state.size();
        int hidden_units = hidden_state[0][0].size();
        Matrix reshaped_matrix(batch_size, std::vector<double>(hidden_units));

        // Extract the last timestep for each example in the batch
        for (int i = 0; i < batch_size; ++i) {
            if (hidden_state[i].empty()) {
                throw std::invalid_argument("Hidden state is empty");
            }

            reshaped_matrix[i] = hidden_state[i].back();  // return the last timestep in the sequence
        }
        return reshaped_matrix;
    }

    //Matrix --> Tensor3D conversion with number of timesteps initialized in x_train
    Tensor3D reshape_last_timestep(const Matrix& hidden_state) {
        int batch_size = hidden_state.size();
        int hidden_units = hidden_state[0].size();
        const int TIMESTEPS = std::get<Tensor3D>(x_train)[0].size();
        Tensor3D reshaped_tensor(batch_size, Matrix(TIMESTEPS, std::vector<double>(hidden_units, 0.0)));

        // Reshape:
        for (int i = 0; i < batch_size; i++) {
            for (int t = 0; t < TIMESTEPS; t++) {
                reshaped_tensor[i][t] = hidden_state[i];
            }
        }

        return reshaped_tensor;
    }

        void forward_prop(std::variant<Tensor3D, Matrix> x_train) {
        /*
        NOTE: Right now, function assumes that the first inputs are LSTMs and last inputs are MLP.
              - e.g: Relu->Relu->LSTM->LSTM is not supported, because LSTMs are placed last
              - e.g: LSTM->LSTM->Relu->Linear is supported, because LSTMs are placed before MLP in the network
              Architectures that are "mixed" is not supported
              - e.g: LSTM->Relu->LSTM->Linear
         */
        Matrix Wy = layer_params[0]["Wy1"];
        int n_a = Wy[0].size();
        std::cout << "HybridModel::forward_prop - n_a (derived from Wy[0].size()): " << n_a << std::endl; // Print n_a

        //MLP
        Matrix a_out;
        bool first_mlp_encountered = false;

        //LSTM
        Matrix a_initial = linalg::generateZeros(std::get<Tensor3D>(x_train).size(), n_a); //Initially, a0 is a Matrix of zeros with shape (m, n_a)
        //std::cout << "Shape of a_initial BEFORE lstm_forward: " << linalg::shape(a_initial) << std::endl;
        Tensor3D new_x_state;
        Tensor3D new_hidden_state;

        std::cout << "Forward prop initialization successful" << std::endl;

        for (int i = 1; i <= layer_types.size(); i++) {
            std::cout << "Layer " << i << ": " << layer_types[i-1] << std::endl;
            if (layer_types[i-1] == "LSTM") {
                if (i == 1) {
                    //Initialize parameters in the function and forward prop through the network once
                    LSTMCache current_lstm_tuple = LSTMNetwork::lstm_forward(std::get<Tensor3D>(x_train), a_initial, layer_params[i-1], i);
                    new_x_state = std::get<1>(std::get<3>(current_lstm_tuple));
                    new_hidden_state = std::get<0>(current_lstm_tuple);
                    
                    if (cache.cache.size() == layer_types.size()) { //Replacing (current iteration != 1st iteration)
                        cache.cache[i] = current_lstm_tuple;
                    } else { //First iteration
                        cache.cache.push_back(current_lstm_tuple);
                    }

                    std::cout << "LSTM forward, layer 1 --> successful" << std::endl;
                } else {
                    LSTMCache current_lstm_tuple = LSTMNetwork::lstm_forward(new_x_state, reshape_last_timestep(new_hidden_state), layer_params[i-1], i);
                    new_x_state = std::get<1>(std::get<3>(current_lstm_tuple));
                    new_hidden_state = std::get<0>(current_lstm_tuple);

                    if (cache.cache.size() == layer_types.size()) { 
                        cache.cache[i] = current_lstm_tuple;
                    } else {
                        cache.cache.push_back(current_lstm_tuple);
                    }

                    std::cout << "LSTM forward, all other layers successful" << std::endl;
                }
            } else if (layer_types[i-1] == "Relu") {
                // Reshape a_out using the last timestepped hidden state from LSTM_forward
                if (layer_types[i-2] == "LSTM" && i != 1) {
                    a_out = reshape_last_timestep(new_hidden_state);
                    first_mlp_encountered = true;
                } else {
                    first_mlp_encountered = false;
                }

                if (i == 1) {
                    //Input x is a Matrix
                    std::tuple<Matrix, matrixDict> current_dense_tuple = MLP::Dense(std::get<Matrix>(x_train), layer_params[i-1], activations::relu, i, first_mlp_encountered);
                    a_out = std::get<0>(current_dense_tuple);
                    matrixDict current_mlp_cache = std::get<1>(current_dense_tuple);

                    if (cache.cache.size() == layer_types.size()) {
                        cache.cache[i] = current_mlp_cache;
                    } else {
                        cache.cache.push_back(current_mlp_cache);
                    }

                } else {
                    std::tuple<Matrix, matrixDict> current_dense_tuple = MLP::Dense(a_out, layer_params[i-1], activations::relu, i, first_mlp_encountered);
                    a_out = std::get<0>(current_dense_tuple);
                    matrixDict current_mlp_cache = std::get<1>(current_dense_tuple);
                    
                    if (cache.cache.size() == layer_types.size()) {
                        cache.cache[i] = current_mlp_cache;
                    } else {
                        cache.cache.push_back(current_mlp_cache);
                    }
                }
            } else if (layer_types[i-1] == "Linear") {
                // Reshape a_out using the last timestepped hidden state from LSTM_forward
                if (layer_types[i-1] == "LSTM" && i != 1) {
                    a_out = reshape_last_timestep(new_hidden_state);
                    first_mlp_encountered = true;
                } else {
                    first_mlp_encountered = false;
                }

                std::tuple<Matrix, matrixDict> current_dense_tuple = MLP::Dense(a_out, layer_params[i-1], activations::linear, i, first_mlp_encountered);
                a_out = std::get<0>(current_dense_tuple);
                matrixDict current_mlp_cache = std::get<1>(current_dense_tuple);
                
                if (cache.cache.size() == layer_types.size()) {
                    cache.cache[i] = current_mlp_cache;
                } else {
                    cache.cache.push_back(current_mlp_cache);
                }
            }
        }
        //Set the final prediction matrix
        finalPrediction = a_out;
    }

    void loss(Matrix y_train) {
        //Automatic transposition to correct shape
        if (finalPrediction.size() == 1 && finalPrediction[0].size() == BATCH_SIZE) {
            finalPrediction = linalg::transpose(finalPrediction);
        }
        if (y_train.size() == 1 && y_train[0].size() == BATCH_SIZE) {
            y_train = linalg::transpose(y_train);
        }

        //Reshape predictions and targets
        std::vector<double> predictions = linalg::reshape(finalPrediction);
        std::vector<double> targets = linalg::reshape(y_train);

        std::cout << predictions.size() << std::endl;
        std::cout << targets.size() << std::endl;

        //predictions and current y_train are of the same mini-batch (BATCH_SIZE = 64):
        accumulated_loss += MSE(predictions, targets);
        //std::cout << "Loss: " << accumulated_loss << std::endl;
    }

    double return_avg_loss() {
        return accumulated_loss / (std::holds_alternative<Tensor3D>(x_train) ? std::get<Tensor3D>(x_train).size() : std::get<Matrix>(x_train).size());
    }

    void back_prop() {
        gradientDict gradients;
        const int L = layer_types.size(); //num of layers
        const int m = std::get<Tensor3D>(x_train).size();
        Matrix a_in_matrix = reshape_last_timestep(std::get<Tensor3D>(x_train));

        // Derivatives
        Matrix dA_matrix;
        //if (std::holds_alternative<matrixDict>(cache.cache[L-1])) {
            //Access the cache at L
        std::cerr << "DEBUG: back_prop - Layer " << L << " cache variant index: " << cache.cache.size() << std::endl; // Print variant index
        matrixDict& layer_cache = std::get<matrixDict>(cache.cache[L-1]);

        // Check if the key exists
        auto item = layer_cache.find("A"+std::to_string(L-1));
        if (item != layer_cache.end()) {
            dA_matrix = item -> second;
        }

        dA_matrix = linalg::division(linalg::subtract(dA_matrix, y_train), m); //Init gradient for the last layer (derivative of loss function)
        //}
        Tensor3D dA_tensor; //To store reshaped LSTM gradients

        for (int layer = L; layer >= 1; layer--) {
            if (layer_types[layer-1] == "LSTM") {
                if (layer == L) {
                    continue; //Skip, assume last layer is always a linear/MLP output
                }

                //Reshape from Matrix to Tensor3D if last backpropagated layer wasn't LSTM with Tensor3D
                if (layer_types[layer-1] == "Relu" || layer_types[layer-1] == "Linear") {
                    dA_tensor = reshape_last_timestep(dA_matrix);
                }

                if (std::holds_alternative<LSTMCache>(cache.cache[layer])) { //Check for correct type
                    //Get the current LSTM cache
                    LSTMCache lstm_cache = std::get<LSTMCache>(cache.cache[layer]);
                    gradientDict current_lstm_grads = LSTMNetwork::lstm_backprop(
                        dA_tensor,
                        std::make_tuple(
                            std::get<0>(std::get<3>(lstm_cache)),  // Extracts the vector<cacheTuple>
                            std::get<1>(std::get<3>(lstm_cache))   // Extracts the Tensor3D
                        ),
                        layer
                    );

                    // Update the new activation derivative
                    dA_tensor = std::get<Tensor3D>(current_lstm_grads["da0"+std::to_string(layer)]);

                    //Store gradients
                    if (grads.grads.size() == layer_types.size()) {
                        grads.grads[layer] = current_lstm_grads;
                    } else {
                        grads.grads.push_back(current_lstm_grads);
                    }
                    
                }

            } else if (layer_types[layer-1] == "Relu" || layer_types[layer-1] == "Linear") {
                if (layer == L) {
                    continue;
                }
                // Reshape dA to a matrix using the last timestepped hidden state from LSTM gradients
                if (layer_types[layer-1] == "LSTM") {
                    dA_matrix = reshape_last_timestep(dA_tensor);
                }

                //Compute gradients
                matrixDict current_mlp_grads = MLP::mlp_backward(
                    a_in_matrix, dA_matrix, y_train,
                    std::get<matrixDict>(cache.cache[layer-1]), layer,
                    (layer_types[layer-1] == "Relu") ? activations::relu : activations::linear); //Ternary operator between Relu and Linear

                //Store gradients
                if (grads.grads.size() == layer_types.size()) {
                    grads.grads[layer] = current_mlp_grads;
                } else {
                    grads.grads.push_back(current_mlp_grads);
                }
            }
        }
    }

    void init_Adam() {
        Adam_params.resize(layer_types.size()); // Initialize Adam_params size
    
        for (int i = 1; i <= layer_types.size(); i++) {
            matrixDict v; //Momentum
            matrixDict s; //Root Mean Square Propagation (RMSP)
            //std::cout << "Layer " << i << ": " << layer_types[i-1] << std::endl;

            if (layer_types[i-1] == "LSTM") {
                // Forget gates
                v["dWf"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wf"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wf"+std::to_string(i)][0].size()));
                v["dbf"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["bf"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["bf"+std::to_string(i)][0].size()));
                s["dWf"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wf"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wf"+std::to_string(i)][0].size()));
                s["dbf"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["bf"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["bf"+std::to_string(i)][0].size()));

                // Update (input) gates
                v["dWi"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wi"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wi"+std::to_string(i)][0].size()));
                v["dbi"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["bi"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["bi"+std::to_string(i)][0].size()));
                s["dWi"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wi"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wi"+std::to_string(i)][0].size()));
                s["dbi"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["bi"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["bi"+std::to_string(i)][0].size()));

                // Candidate/memory cells
                v["dWc"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wc"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wc"+std::to_string(i)][0].size()));
                v["dbc"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["bc"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["bc"+std::to_string(i)][0].size()));
                s["dWc"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wc"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wc"+std::to_string(i)][0].size()));
                s["dbc"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["bc"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["bc"+std::to_string(i)][0].size()));

                //Output gates
                v["dWo"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wo"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wo"+std::to_string(i)][0].size()));
                v["dbo"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["bo"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["bo"+std::to_string(i)][0].size()));
                s["dWo"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wo"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wo"+std::to_string(i)][0].size()));
                s["dbo"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["bo"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["bo"+std::to_string(i)][0].size()));

                //Predictions
                v["dWy"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wy"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wy"+std::to_string(i)][0].size()));
                v["dby"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["by"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["by"+std::to_string(i)][0].size()));
                s["dWy"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["Wy"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["Wy"+std::to_string(i)][0].size()));
                s["dby"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["by"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["by"+std::to_string(i)][0].size()));

            } else if (layer_types[i-1] == "Relu" || layer_types[i-1] == "Linear") {
                v["dW"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["W"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["W"+std::to_string(i)][0].size()));
                v["db"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["b"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["b"+std::to_string(i)][0].size()));
                s["dW"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["W"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["W"+std::to_string(i)][0].size()));
                s["db"+std::to_string(i)] = linalg::generateZeros(static_cast<int>(layer_params[i-1]["b"+std::to_string(i)].size()), static_cast<int>(layer_params[i-1]["b"+std::to_string(i)][0].size()));
            }
            Adam_params[i-1] = {v, s};
        }
        std::cout << "Adam parameter initialization successful" << std::endl;
    }

    void optimize() {
        for (int l = 1; l <= layer_types.size(); l++) {
            matrixDict v = Adam_params[l][0];
            matrixDict s = Adam_params[l][1];
            matrixDict v_corrected = {};
            matrixDict s_corrected = {};

            if (layer_types[l-1] == "LSTM") {
                // Calculate momentums with beta1
                auto& grad_map = std::get<gradientDict>(grads.grads[l-1]);
                v["dWf"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dWf"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dWf"+std::to_string(l+1)])));
                v["dbf"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dbf"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dbf"+std::to_string(l+1)])));
                v["dWi"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dWi"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dWi"+std::to_string(l+1)])));
                v["dbi"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dbi"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dbi"+std::to_string(l+1)])));
                v["dWc"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dWc"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dWc"+std::to_string(l+1)])));
                v["dbc"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dbc"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dbc"+std::to_string(l+1)])));
                v["dWo"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dWo"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dWo"+std::to_string(l+1)])));
                v["dbo"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dbo"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dbo"+std::to_string(l+1)])));
                v["dWy"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dWy"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dWy"+std::to_string(l+1)])));
                v["dby"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dby"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dby"+std::to_string(l+1)])));

                // Calculate corrected v values:
                v_corrected["dWf"+std::to_string(l+1)] = linalg::division(v["dWf"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["dbf"+std::to_string(l+1)] = linalg::division(v["dbf"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["dWi"+std::to_string(l+1)] = linalg::division(v["dWi"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["dbi"+std::to_string(l+1)] = linalg::division(v["dbi"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["dWc"+std::to_string(l+1)] = linalg::division(v["dWc"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["dbc"+std::to_string(l+1)] = linalg::division(v["dbc"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["dWo"+std::to_string(l+1)] = linalg::division(v["dWo"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["dbo"+std::to_string(l+1)] = linalg::division(v["dbo"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["dWy"+std::to_string(l+1)] = linalg::division(v["dWy"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["dby"+std::to_string(l+1)] = linalg::division(v["dby"+std::to_string(l+1)], (1-std::pow(beta1, t)));

                // Calculate the RMSProps with beta2
                s["dWf"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dWf"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dWf"+std::to_string(l+1)]), 2.0)));
                s["dbf"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dbf"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dbf"+std::to_string(l+1)]), 2.0)));
                s["dWi"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dWi"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dWi"+std::to_string(l+1)]), 2.0)));
                s["dbi"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dbi"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dbi"+std::to_string(l+1)]), 2.0)));
                s["dWc"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dWc"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dWc"+std::to_string(l+1)]), 2.0)));
                s["dbc"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dbc"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dbc"+std::to_string(l+1)]), 2.0)));
                s["dWo"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dWo"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dWo"+std::to_string(l+1)]), 2.0)));
                s["dbo"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dbo"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dbo"+std::to_string(l+1)]), 2.0)));
                s["dWy"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dWy"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dWy"+std::to_string(l+1)]), 2.0)));
                s["dby"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dby"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dby"+std::to_string(l+1)]), 2.0)));

                // Calculate corrected s values:
                s_corrected["dWf"+std::to_string(l+1)] = linalg::division(s["dWf"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["dbf"+std::to_string(l+1)] = linalg::division(s["dbf"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["dWi"+std::to_string(l+1)] = linalg::division(s["dWi"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["dbi"+std::to_string(l+1)] = linalg::division(s["dbi"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["dWc"+std::to_string(l+1)] = linalg::division(s["dWc"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["dbc"+std::to_string(l+1)] = linalg::division(s["dbc"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["dWo"+std::to_string(l+1)] = linalg::division(s["dWo"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["dbo"+std::to_string(l+1)] = linalg::division(s["dbo"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["dWy"+std::to_string(l+1)] = linalg::division(s["dWy"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["dby"+std::to_string(l+1)] = linalg::division(s["dby"+std::to_string(l+1)], (1-std::pow(beta2, t)));

                // Update parameters
                layer_params[l-1]["Wf"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["Wf"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dWf"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dWf"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["bf"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["bf"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dbf"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dbf"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["Wi"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["Wi"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dWi"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dWi"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["bi"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["bi"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dbi"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dbi"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["Wc"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["Wc"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dWc"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dWc"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["bc"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["bc"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dbc"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dbc"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["Wo"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["Wo"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dWo"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dWo"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["bo"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["bo"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dbo"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dbo"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["Wy"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["Wy"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dWy"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dWy"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["by"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["by"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dby"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dby"+std::to_string(l+1)]), epsilon)));
            
            } else if (layer_types[l-1] == "Relu" || layer_types[l-1] == "Linear") {
                // Calculate momentums with beta1
                auto& grad_map = std::get<gradientDict>(grads.grads[l-1]);
                v["dW"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["dW"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["dW"+std::to_string(l+1)])));
                v["db"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta1, v["db"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta1), std::get<Matrix>(grad_map["db"+std::to_string(l+1)])));

                // Calculate corrected v values:
                v_corrected["dW"+std::to_string(l+1)] = linalg::division(v["dW"+std::to_string(l+1)], (1-std::pow(beta1, t)));
                v_corrected["db"+std::to_string(l+1)] = linalg::division(v["db"+std::to_string(l+1)], (1-std::pow(beta1, t)));

                // Calculate the RMSProps with beta2
                s["dW"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["dW"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["dW"+std::to_string(l+1)]), 2.0)));
                s["db"+std::to_string(l+1)] = linalg::add(linalg::scalarMultiply(beta2, s["db"+std::to_string(l+1)]), linalg::scalarMultiply((1-beta2), linalg::pow(std::get<Matrix>(grad_map["db"+std::to_string(l+1)]), 2.0)));

                // Calculate corrected s values:
                s_corrected["dW"+std::to_string(l+1)] = linalg::division(s["dW"+std::to_string(l+1)], (1-std::pow(beta2, t)));
                s_corrected["db"+std::to_string(l+1)] = linalg::division(s["db"+std::to_string(l+1)], (1-std::pow(beta2, t)));

                // Update parameters
                layer_params[l-1]["W"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["W"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["dW"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["dW"+std::to_string(l+1)]), epsilon)));
                layer_params[l-1]["b"+std::to_string(l+1)] = linalg::subtract(layer_params[l-1]["b"+std::to_string(l+1)], linalg::division(linalg::scalarMultiply(learning_rate, v_corrected["db"+std::to_string(l+1)]), linalg::add(linalg::sqrt(s_corrected["db"+std::to_string(l+1)]), epsilon)));
            }
        }
        t += 1;
    }
}
