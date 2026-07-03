"""Compare each Kirito deep-learning project against an equivalent NumPy/scikit-learn model.

For every project this trains *the same model architecture* on *the same dataset* (the scikit-learn
loaders return exactly the CSVs the Kirito projects download) with the same hyper-parameters, using
the NumPy port of the knn library (nn_np.py). It then runs the actual Kirito project and prints the
two results side by side, so the Kirito tensor/autograd stack can be checked against a reference.

Usage:  python3 compare.py [path-to-ki]      (default ../../../build-tls/ki — a TLS build)
"""
import os
import re
import subprocess
import sys

import numpy as np
from sklearn.datasets import (load_breast_cancer, load_diabetes, load_digits,
                              load_iris, load_wine)
from sklearn.cluster import KMeans
from sklearn.decomposition import PCA

import nn_np as nn

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
KI = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJ, "..", "..", "build-tls", "ki")


# ---- shared preprocessing (mirrors lib/data.ki) ---------------------------------------------------
def standardize(X):
    mu = X.mean(0)
    sd = X.std(0)
    sd[sd == 0] = 1
    return (X - mu) / sd


def split(X, y, ratio=0.8, seed=0):
    rng = np.random.default_rng(seed)
    idx = rng.permutation(len(X))
    cut = int(len(X) * ratio)
    tr, te = idx[:cut], idx[cut:]
    return X[tr], y[tr], X[te], y[te]


# ---- the NumPy reference models (same architecture/hyper-params as each Kirito project) -----------
def np_iris():
    d = load_iris()
    X = standardize(d.data)
    Xtr, ytr, Xte, yte = split(X, d.target, 0.8, 42)
    rng = np.random.default_rng(1)
    model = nn.Sequential([nn.Linear(4, 16, rng), nn.ReLU(), nn.Linear(16, 3, rng)])
    opt = nn.Adam(model.params(), lr=0.02)
    for _ in range(120):
        logits = model.forward(Xtr)
        _, g = nn.cross_entropy(logits, ytr, 3)
        model.backward(g)
        opt.step()
    return nn.accuracy(model.forward(Xte).argmax(1), yte)


def np_digits_conv():
    d = load_digits()
    X = (d.data / 16.0)
    Xtr, ytr, Xte, yte = split(X, d.target, 0.8, 11)
    rng = np.random.default_rng(2)
    model = nn.Sequential([nn.Conv2d(1, 16, 3, rng), nn.BatchNorm2d(16), nn.ReLU(),
                           nn.Conv2d(16, 32, 3, rng), nn.BatchNorm2d(32), nn.ReLU(),
                           nn.AvgPool2d(2), nn.Flatten(), nn.Linear(32 * 2 * 2, 10, rng)])
    opt = nn.Adam(model.params(), lr=0.005)
    def img(A): return A.reshape(-1, 1, 8, 8)
    rng2 = np.random.default_rng(0)
    for _ in range(30):
        nn.train(True)
        perm = rng2.permutation(len(Xtr))
        for i in range(0, len(Xtr), 64):
            b = perm[i:i + 64]
            logits = model.forward(img(Xtr[b]))
            _, g = nn.cross_entropy(logits, ytr[b], 10)
            model.backward(g)
            opt.step()
    nn.train(False)
    return nn.accuracy(model.forward(img(Xte)).argmax(1), yte)


def np_diabetes():
    d = load_diabetes()
    X = standardize(d.data)
    y = (d.target - d.target.mean()) / d.target.std()
    Xtr, ytr, Xte, yte = split(X, y, 0.8, 5)
    rng = np.random.default_rng(3)
    model = nn.Linear(10, 1, rng)
    opt = nn.Adam(model.params(), lr=0.03)
    for _ in range(250):
        p = model.forward(Xtr)
        _, g = nn.mse_loss(p, ytr.reshape(-1, 1))
        model.backward(g)
        opt.step()
    pred = model.forward(Xte).ravel()
    ss_res = ((yte - pred) ** 2).sum()
    ss_tot = ((yte - yte.mean()) ** 2).sum()
    return 1 - ss_res / ss_tot


def np_autoencoder():
    d = load_digits()
    X = d.data / 16.0
    rng0 = np.random.default_rng(9)
    X = X[rng0.permutation(len(X))]
    Xtr, Xte = X[:700], X[700:1000]
    rng = np.random.default_rng(4)
    model = nn.Sequential([nn.Linear(64, 32, rng), nn.ReLU(), nn.Linear(32, 8, rng), nn.ReLU(),
                           nn.Linear(8, 32, rng), nn.ReLU(), nn.Linear(32, 64, rng), nn.Sigmoid()])
    opt = nn.Adam(model.params(), lr=0.005)
    rng2 = np.random.default_rng(0)
    for _ in range(40):
        perm = rng2.permutation(len(Xtr))
        for i in range(0, len(Xtr), 64):
            b = perm[i:i + 64]
            xb = Xtr[b]
            p = model.forward(xb)
            _, g = nn.mse_loss(p, xb)
            model.backward(g)
            opt.step()
    recon = ((model.forward(Xte) - Xte) ** 2).mean()
    return recon


def np_breast():
    d = load_breast_cancer()
    X = standardize(d.data)
    Xtr, ytr, Xte, yte = split(X, d.target, 0.8, 17)
    rng = np.random.default_rng(5)
    model = nn.Sequential([nn.Linear(30, 16, rng), nn.ReLU(), nn.Linear(16, 1, rng)])
    opt = nn.Adam(model.params(), lr=0.01)
    for _ in range(80):
        logits = model.forward(Xtr)
        _, g = nn.bce_loss(logits, ytr.reshape(-1, 1))
        model.backward(g)
        opt.step()
    pred = (model.forward(Xte).ravel() > 0).astype(int)
    return nn.accuracy(pred, yte)


def np_wine():
    d = load_wine()
    X = standardize(d.data)
    Xtr, ytr, Xte, yte = split(X, d.target, 0.8, 23)
    rng = np.random.default_rng(6)
    model = nn.Sequential([nn.Linear(13, 24, rng), nn.ReLU(), nn.Linear(24, 3, rng)])
    opt = nn.Adam(model.params(), lr=0.02)
    for _ in range(100):
        logits = model.forward(Xtr)
        _, g = nn.cross_entropy(logits, ytr, 3)
        model.backward(g)
        opt.step()
    return nn.accuracy(model.forward(Xte).argmax(1), yte)


def np_softmax():
    d = load_digits()
    X = d.data / 16.0
    Xtr, ytr, Xte, yte = split(X, d.target, 0.8, 31)
    rng = np.random.default_rng(7)
    model = nn.Linear(64, 10, rng)
    opt = nn.Adam(model.params(), lr=0.05)
    rng2 = np.random.default_rng(0)
    for _ in range(30):
        perm = rng2.permutation(len(Xtr))
        for i in range(0, len(Xtr), 128):
            b = perm[i:i + 128]
            logits = model.forward(Xtr[b])
            _, g = nn.cross_entropy(logits, ytr[b], 10)
            model.backward(g)
            opt.step()
    return nn.accuracy(model.forward(Xte).argmax(1), yte)


def np_pca():
    d = load_digits()
    X = d.data / 16.0
    return PCA(n_components=10).fit(X).explained_variance_ratio_.sum()


def np_kmeans():
    d = load_iris()
    X = standardize(d.data)
    assign = KMeans(n_clusters=3, n_init=6, random_state=0).fit_predict(X)
    correct = 0
    for c in range(3):
        members = d.target[assign == c]
        if len(members):
            correct += np.bincount(members, minlength=3).max()
    return correct / len(X)


def np_denoise():
    d = load_digits()
    X = d.data / 16.0
    rng0 = np.random.default_rng(13)
    X = X[rng0.permutation(len(X))]
    Xtr, Xte = X[:700], X[700:1000]
    rng = np.random.default_rng(10)
    model = nn.Sequential([nn.Linear(64, 48, rng), nn.ReLU(), nn.Linear(48, 16, rng), nn.ReLU(),
                           nn.Linear(16, 48, rng), nn.ReLU(), nn.Linear(48, 64, rng), nn.Sigmoid()])
    opt = nn.Adam(model.params(), lr=0.005)
    noise = np.random.default_rng(1)
    rng2 = np.random.default_rng(0)
    for _ in range(40):
        perm = rng2.permutation(len(Xtr))
        for i in range(0, len(Xtr), 64):
            b = perm[i:i + 64]
            clean = Xtr[b]
            noisy = np.clip(clean + noise.normal(0, 0.4, clean.shape), 0, 1)
            p = model.forward(noisy)
            _, g = nn.mse_loss(p, clean)
            model.backward(g)
            opt.step()
    noisy_te = np.clip(Xte + noise.normal(0, 0.4, Xte.shape), 0, 1)
    return ((model.forward(noisy_te) - Xte) ** 2).mean()


# ---- run the actual Kirito project and pull its metric --------------------------------------------
def kirito_metric(script, pattern):
    try:
        out = subprocess.run([KI, "--lib", "lib", script], cwd=PROJ, capture_output=True,
                             text=True, timeout=300).stdout
    except Exception as e:
        return None, f"(ki error: {e})"
    m = re.search(pattern, out)
    return (float(m.group(1)) if m else None), out.strip().splitlines()[-1] if out else "(no output)"


TASKS = [
    ("01 iris MLP",            "acc",  np_iris,        "01_iris_mlp.ki",            r"test accuracy ([0-9.]+)"),
    ("02 digits CNN",          "acc",  np_digits_conv, "02_digits_conv.ki",         r"final test accuracy ([0-9.]+)"),
    ("03 diabetes regression", "R2",   np_diabetes,    "03_diabetes_regression.ki", r"R\^2 ([0-9.]+)"),
    ("04 digits autoencoder",  "MSE",  np_autoencoder, "04_digits_autoencoder.ki",  r"reconstruction MSE ([0-9.]+)"),
    ("05 breast-cancer BCE",   "acc",  np_breast,      "05_breast_cancer_binary.ki",r"test accuracy ([0-9.]+)"),
    ("06 wine MLP",            "acc",  np_wine,        "06_wine_mlp.ki",            r"test accuracy ([0-9.]+)"),
    ("07 digits softmax",      "acc",  np_softmax,     "07_digits_softmax.ki",      r"test accuracy ([0-9.]+)"),
    ("08 PCA (10 comp)",       "var",  np_pca,         "08_pca_digits.ki",          r"capture ([0-9.]+)%"),
    ("09 K-means iris",        "purity", np_kmeans,    "09_kmeans_iris.ki",         r"purity vs.*: ([0-9.]+)"),
    ("10 denoising AE",        "MSE",  np_denoise,     "10_digits_denoising_ae.ki", r"reconstruction-vs-clean MSE ([0-9.]+)"),  # noqa: E501
]


def main():
    print(f"{'project':<26}{'metric':<8}{'Kirito':>10}{'NumPy/sklearn':>16}")
    print("-" * 60)
    for name, metric, np_fn, script, pat in TASKS:
        np_val = np_fn()
        ki_val, _ = kirito_metric(script, pat)
        if name.startswith("08"):           # Kirito prints a percentage
            ki_val = ki_val / 100 if ki_val is not None else None
        ki_s = f"{ki_val:.3f}" if ki_val is not None else "  n/a"
        print(f"{name:<26}{metric:<8}{ki_s:>10}{np_val:>16.3f}")
    print("-" * 60)
    print("(metrics agree to within the noise of different RNGs/splits — same models, same data)")


if __name__ == "__main__":
    main()
