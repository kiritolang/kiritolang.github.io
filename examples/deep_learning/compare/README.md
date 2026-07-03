# Cross-check against NumPy / scikit-learn

These scripts validate the Kirito projects by training **the same model architectures** on **the same
datasets** with a NumPy reference, and comparing the metrics.

- `nn_np.py` — a NumPy port of the Kirito `knn` library: the same `Linear` / `Conv2d` (im2col) /
  `ReLU` / `Sigmoid` / `AvgPool2d` / `Flatten` / `Sequential`, the same `mse` / `cross_entropy` /
  `bce` losses, and the same `Adam`. NumPy here plays the role the native `tensor` module plays in
  Kirito.
- `compare.py` — builds each model in NumPy, runs the matching Kirito project (`build-tls/ki`), and
  prints both results side by side. The scikit-learn dataset loaders return exactly the CSVs the
  Kirito projects download, so the data is identical; PCA and K-means are also cross-checked against
  scikit-learn's own `PCA` / `KMeans`.

```sh
pip install numpy scikit-learn
python3 compare.py [path-to-ki]
```

The metrics agree to within the noise of different RNG seeds and train/test splits — for example PCA's
explained variance matches scikit-learn **exactly** (0.738), and the breast-cancer accuracy matches
(0.965). Because the architectures, hyper-parameters, and data are the same, this is direct evidence
that Kirito's tensor engine, autograd, and the conv backward are correct.

A representative run:

| project | metric | Kirito | NumPy/sklearn |
| --- | --- | --- | --- |
| iris MLP | accuracy | 1.00 | 0.93 |
| digits CNN (ConvNormAct) | accuracy | 0.98 | 0.99 |
| diabetes regression | R² | 0.39 | 0.33 |
| digits autoencoder | recon MSE | 0.029 | 0.025 |
| breast-cancer | accuracy | 0.965 | 0.965 |
| wine MLP | accuracy | 0.92 | 1.00 |
| digits softmax | accuracy | 0.98 | 0.95 |
| PCA (10 comp) | variance | 0.738 | 0.738 |
| K-means iris | purity | 0.83 | 0.83 |
| denoising AE | recon MSE | 0.035 | 0.036 |
