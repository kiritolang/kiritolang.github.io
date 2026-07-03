"""A faithful NumPy reimplementation of the Kirito `knn` library (examples/deep_learning/lib/nn.ki).

Same layers, losses, optimizers, and the same im2col convolution — so the Python comparison scripts
build *the same models* as the Kirito projects. NumPy here plays the role the native `tensor` module
plays in Kirito; the architectures, hyper-parameters, and datasets are identical.
"""
import numpy as np

# Global train/eval mode (mirrors nn.ki's nn.train(flag)); BatchNorm consults it.
TRAINING = [True]


def train(flag):
    TRAINING[0] = flag


class Parameter:
    def __init__(self, t):
        self.t = t
        self.grad = None


class Linear:
    def __init__(self, nin, nout, rng):
        self.W = Parameter(rng.standard_normal((nin, nout)) * np.sqrt(2.0 / nin))  # He init
        self.b = Parameter(np.zeros(nout))

    def forward(self, x):
        self.x = x
        return x @ self.W.t + self.b.t

    def backward(self, g):
        self.W.grad = self.x.reshape(-1, self.x.shape[-1]).T @ g.reshape(-1, g.shape[-1])
        self.b.grad = g.reshape(-1, g.shape[-1]).sum(0)
        return g @ self.W.t.T

    def params(self):
        return [self.W, self.b]


class ReLU:
    def forward(self, x):
        self.m = x > 0
        return x * self.m

    def backward(self, g):
        return g * self.m

    def params(self):
        return []


class Sigmoid:
    def forward(self, x):
        self.y = 1.0 / (1.0 + np.exp(-x))
        return self.y

    def backward(self, g):
        return g * self.y * (1 - self.y)

    def params(self):
        return []


class Flatten:
    def forward(self, x):
        self.shape = x.shape
        return x.reshape(x.shape[0], -1)

    def backward(self, g):
        return g.reshape(self.shape)

    def params(self):
        return []


class Conv2d:
    """Stride-1, valid convolution via im2col — the same scheme as the Kirito Conv2d."""

    def __init__(self, cin, cout, k, rng):
        scale = np.sqrt(2.0 / (cin * k * k))
        self.W = Parameter(rng.standard_normal((cout, cin, k, k)) * scale)
        self.b = Parameter(np.zeros(cout))
        self.cin, self.cout, self.k = cin, cout, k

    def _cols(self, x):
        n, c, h, w = x.shape
        k = self.k
        ho, wo = h - k + 1, w - k + 1
        cols = np.empty((n, ho * wo, c * k * k))
        idx = 0
        for di in range(k):
            for dj in range(k):
                patch = x[:, :, di:di + ho, dj:dj + wo]          # (n, c, ho, wo)
                cols[:, :, idx * c:(idx + 1) * c] = patch.reshape(n, c, ho * wo).transpose(0, 2, 1)
                idx += 1
        return cols, ho, wo

    def forward(self, x):
        self.x = x
        n = x.shape[0]
        cols, ho, wo = self._cols(x)
        self.cols, self.ho, self.wo = cols, ho, wo
        wflat = self.W.t.transpose(2, 3, 1, 0).reshape(-1, self.cout)   # (k*k*cin, cout)
        out = cols.reshape(n * ho * wo, -1) @ wflat + self.b.t
        return out.reshape(n, ho, wo, self.cout).transpose(0, 3, 1, 2)

    def backward(self, g):
        n = self.x.shape[0]
        gflat = g.transpose(0, 2, 3, 1).reshape(n * self.ho * self.wo, self.cout)
        wflat = self.W.t.transpose(2, 3, 1, 0).reshape(-1, self.cout)
        self.b.grad = gflat.sum(0)
        gw = self.cols.reshape(n * self.ho * self.wo, -1).T @ gflat       # (k*k*cin, cout)
        self.W.grad = gw.reshape(self.k, self.k, self.cin, self.cout).transpose(3, 2, 0, 1)
        # input gradient via col2im (needed when a conv feeds another conv)
        dcols = (gflat @ wflat.T).reshape(n, self.ho * self.wo, -1)
        dx = np.zeros_like(self.x)
        c, idx = self.cin, 0
        for di in range(self.k):
            for dj in range(self.k):
                patch = dcols[:, :, idx * c:(idx + 1) * c].transpose(0, 2, 1).reshape(n, c, self.ho, self.wo)
                dx[:, :, di:di + self.ho, dj:dj + self.wo] += patch
                idx += 1
        return dx

    def params(self):
        return [self.W, self.b]


class BatchNorm2d:
    """Batch norm over (N, H, W) per channel, with the standard compact backward — matching the
    Kirito BatchNorm2d (batch stats + running averages, train/eval via the global TRAINING flag)."""

    def __init__(self, c, momentum=0.1, eps=1e-5):
        self.gamma = Parameter(np.ones(c))
        self.beta = Parameter(np.zeros(c))
        self.running_mean = np.zeros(c)
        self.running_var = np.ones(c)
        self.momentum, self.eps = momentum, eps

    def forward(self, x):
        if TRAINING[0]:
            mean = x.mean(axis=(0, 2, 3))
            var = x.var(axis=(0, 2, 3))
            self.running_mean = (1 - self.momentum) * self.running_mean + self.momentum * mean
            self.running_var = (1 - self.momentum) * self.running_var + self.momentum * var
        else:
            mean, var = self.running_mean, self.running_var
        self.std = np.sqrt(var.reshape(1, -1, 1, 1) + self.eps)
        self.xhat = (x - mean.reshape(1, -1, 1, 1)) / self.std
        return self.xhat * self.gamma.t.reshape(1, -1, 1, 1) + self.beta.t.reshape(1, -1, 1, 1)

    def backward(self, g):
        axes = (0, 2, 3)
        m = g.shape[0] * g.shape[2] * g.shape[3]
        self.gamma.grad = (g * self.xhat).sum(axes)
        self.beta.grad = g.sum(axes)
        gxhat = g * self.gamma.t.reshape(1, -1, 1, 1)
        return (1.0 / self.std / m) * (m * gxhat - gxhat.sum(axes).reshape(1, -1, 1, 1)
                                       - self.xhat * (gxhat * self.xhat).sum(axes).reshape(1, -1, 1, 1))

    def params(self):
        return [self.gamma, self.beta]


class AvgPool2d:
    def __init__(self, k):
        self.k = k

    def forward(self, x):
        n, c, h, w = x.shape
        k = self.k
        self.shape = x.shape
        return x.reshape(n, c, h // k, k, w // k, k).mean(axis=(3, 5))

    def backward(self, g):
        n, c, h, w = self.shape
        k = self.k
        return np.repeat(np.repeat(g, k, axis=2), k, axis=3) / (k * k)

    def params(self):
        return []


class Sequential:
    def __init__(self, layers):
        self.layers = layers

    def forward(self, x):
        for l in self.layers:
            x = l.forward(x)
        return x

    def backward(self, g):
        for l in reversed(self.layers):
            g = l.backward(g)

    def params(self):
        return [p for l in self.layers for p in l.params()]


def mse_loss(pred, target):
    d = pred - target
    return (d * d).mean(), 2 * d / d.size


def cross_entropy(logits, targets, num_classes):
    z = logits - logits.max(1, keepdims=True)
    e = np.exp(z)
    sm = e / e.sum(1, keepdims=True)
    n = logits.shape[0]
    loss = -np.log(sm[np.arange(n), targets] + 1e-12).mean()
    grad = sm.copy()
    grad[np.arange(n), targets] -= 1
    return loss, grad / n


def bce_loss(logits, target):
    p = 1.0 / (1.0 + np.exp(-logits))
    loss = np.mean(np.logaddexp(0, logits) - logits * target)
    return loss, (p - target) / logits.size


class Adam:
    def __init__(self, params, lr=0.001, b1=0.9, b2=0.999, eps=1e-8):
        self.params, self.lr, self.b1, self.b2, self.eps = params, lr, b1, b2, eps
        self.m = [np.zeros_like(p.t) for p in params]
        self.v = [np.zeros_like(p.t) for p in params]
        self.t = 0

    def step(self):
        self.t += 1
        for i, p in enumerate(self.params):
            if p.grad is None:
                continue
            self.m[i] = self.b1 * self.m[i] + (1 - self.b1) * p.grad
            self.v[i] = self.b2 * self.v[i] + (1 - self.b2) * p.grad ** 2
            mh = self.m[i] / (1 - self.b1 ** self.t)
            vh = self.v[i] / (1 - self.b2 ** self.t)
            p.t -= self.lr * mh / (np.sqrt(vh) + self.eps)


def accuracy(pred, truth):
    return float(np.mean(np.asarray(pred) == np.asarray(truth)))
