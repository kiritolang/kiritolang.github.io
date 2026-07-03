# Deep learning in pure Kirito

A small **PyTorch-like deep-learning library** (`lib/nn.ki`) written in pure Kirito on top of the
native `tensor` module's reverse-mode autograd, plus **ten worked projects** that train real models on
real, downloaded datasets. Everything — the layers, losses, optimizers, the conv, the data download
and CSV/gzip parsing — is Kirito. Dimensions are kept small so each project trains in a few seconds on
the Kirito interpreter.

## The library — `lib/nn.ki`

Models are built from composable modules, exactly like PyTorch:

- **Layers:** `Linear`, `Conv2d` (a differentiable im2col convolution), `BatchNorm2d`, `AvgPool2d`,
  `Flatten`, and the activations `ReLU` / `Sigmoid` / `Tanh`. `Sequential([...])` chains them; the
  ConvNormAct block is `Conv2d -> BatchNorm2d -> ReLU`. `nn.train(flag)` toggles train/eval mode (for
  BatchNorm's batch-vs-running statistics).
- **Losses:** `mse_loss`, `cross_entropy` (softmax + NLL, numerically stable), `bce_loss` (BCE from
  logits).
- **Optimizers:** `SGD` (with momentum) and `Adam`. Each `step()` reads the parameters' `.grad`
  (filled by `loss.backward()`) and updates them.
- **Helpers:** `argmax_rows`, `accuracy`, `onehot`, `save_weights` / `load_weights` (checkpoint a
  model via the `serialize` module).

A model is any object exposing `forward(x)` (and `_call_`, so `model(x)` works) and `parameters()` (a
flat List of `Parameter`). The `Conv2d` backward is gradient-checked against finite differences.

`lib/data.ki` downloads, caches, and preprocesses the datasets: `fetch_text` (HTTPS download +
gzip-decompress + on-disk cache), `parse_csv`, `standardize`, `scale_range`, `train_test_split`,
`minibatches`, and `load_iris` / `load_wine` / `load_breast_cancer` / `load_digits` / `load_diabetes`
(the canonical scikit-learn datasets).

## The projects

| # | File | Task | Data |
| - | ---- | ---- | ---- |
| 1 | `01_iris_mlp.ki` | MLP classification on 1-D features | Iris (150×4, 3 classes) |
| 2 | `02_digits_conv.ki` | **CNN** image classification (MNIST-style, two ConvNormAct blocks → ~98%) | Digits (8×8, 10 classes) |
| 3 | `03_diabetes_regression.ki` | Linear regression (RMSE / R²) | Diabetes (442×10) |
| 4 | `04_digits_autoencoder.ki` | Autoencoder (64→8→64 reconstruction) | Digits |
| 5 | `05_breast_cancer_binary.ki` | Binary classification with BCE | Breast cancer (569×30) |
| 6 | `06_wine_mlp.ki` | Multi-class MLP | Wine (178×13, 3 classes) |
| 7 | `07_digits_softmax.ki` | Softmax (multinomial logistic) regression | Digits |
| 8 | `08_pca_digits.ki` | **PCA** by power iteration (pure tensor) | Digits |
| 9 | `09_kmeans_iris.ki` | **K-means** clustering (pure tensor) | Iris |
| 10 | `10_digits_denoising_ae.ki` | Denoising autoencoder | Digits |

Projects 8 and 9 use the tensor library directly (no neural net) — they exercise `matmul`, `outer`,
`trace`, `argmin`, broadcasting, and the reductions.

## Running

The data is fetched over **HTTPS**, so you need a TLS-enabled `ki`:

```sh
cmake -S . -B build-tls -DKIRITO_ENABLE_TLS=ON && cmake --build build-tls --target ki
cd examples/deep_learning
../../build-tls/ki --lib lib 01_iris_mlp.ki
```

Datasets are cached under `.cache/` on first download, so later runs are offline and fast. Run every
project and check each prints its `OK` line:

```sh
./run_all.sh ../../build-tls/ki
```
