"""Paints WinePS4's default desktop photos (app/wallpapers/*.jpg, 1920x1080): original scenes drawn
from noise and gradients, so the package carries no one else's pictures.
  python scripts/make-wallpapers.py"""
import pathlib

import numpy as np
from PIL import Image, ImageFilter

W, H = 1920, 1080
OUT = pathlib.Path(__file__).resolve().parent.parent / 'app' / 'wallpapers'
OUT.mkdir(parents=True, exist_ok=True)
rng = np.random.default_rng(20261101)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
u, v = xx / W, yy / H


def ridge(n, octaves=6, smooth=1.0):
    """A 1D fractal line, n samples, about -1..1 (value noise)"""
    out = np.zeros(n, np.float32)
    amp, freq = 1.0, 3.0
    for _ in range(octaves):
        pts = rng.uniform(-1, 1, int(freq) + 2)
        x = np.linspace(0, freq, n)
        i = x.astype(int)
        t = x - i
        t = t * t * (3 - 2 * t)
        out += amp * (pts[i] * (1 - t) + pts[i + 1] * t)
        amp *= 0.5 * smooth
        freq *= 2.1
    return out / 1.6


def gradient(stops, t):
    """colours at t (0..1) from [(pos, (r, g, b))]"""
    t = np.clip(t, 0, 1)
    out = np.zeros(t.shape + (3,), np.float32)
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        m = (t >= p0) & (t <= p1)
        k = ((t - p0) / max(p1 - p0, 1e-6))[..., None]
        out[m] = (np.array(c0, np.float32) * (1 - k) + np.array(c1, np.float32) * k)[m]
    return out


def mix(img, color, alpha):
    a = np.clip(alpha, 0, 1)[..., None]
    return img * (1 - a) + np.array(color, np.float32) * a


def save(img, name, blur=0.6):
    pic = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    if blur:
        pic = pic.filter(ImageFilter.GaussianBlur(blur))
    noise = rng.normal(0, 1.6, (H, W, 3))  # a little grain, against banding
    pic = Image.fromarray(np.clip(np.asarray(pic, np.float32) + noise, 0, 255).astype(np.uint8))
    pic.save(OUT / name, quality=90)
    print(OUT / name)


def mountain_lake():
    sky = gradient([(0, (36, 58, 112)), (0.35, (122, 120, 170)), (0.55, (246, 178, 140)), (0.6, (255, 214, 170)), (1, (255, 214, 170))], v)
    sun = np.exp(-(((u - 0.62) * 16) ** 2 + ((v - 0.55) * 28) ** 2))
    sky = mix(sky, (255, 236, 200), sun * 0.9)
    img = sky.copy()
    horizon = 0.62
    layers = [(0.34, (92, 92, 140), 0.10), (0.42, (70, 72, 118), 0.13), (0.50, (50, 54, 96), 0.15), (0.57, (30, 36, 70), 0.16)]
    for base, col, height in layers:
        line = base + height * (0.5 - 0.5 * ridge(W, smooth=1.1))
        top = (line * H).astype(int)
        mask = yy >= top[None, :]
        mask &= yy < horizon * H
        fog = np.clip((yy - top[None, :]) / (H * 0.18), 0, 1)
        shade = np.array(col, np.float32) * (1 - 0.35 * fog[..., None]) + np.array((246, 190, 160), np.float32) * 0.35 * fog[..., None] * 0.6
        img[mask] = shade[mask]
    # the lake: the sky and hills mirrored, darker, with ripples
    rows = np.arange(int(horizon * H), H)
    src = (2 * horizon * H - rows).astype(int).clip(0, H - 1)
    ripple = (np.sin(xx[rows] * 0.08 + rows[:, None] * 0.9) * 3).astype(int)
    xs = (xx[rows].astype(int) + ripple).clip(0, W - 1)
    img[rows] = img[src[:, None], xs] * 0.72 + np.array((20, 30, 60), np.float32) * 0.28
    save(img, 'mountain-lake.jpg')


def dunes():
    sky = gradient([(0, (86, 140, 205)), (0.45, (190, 210, 230)), (0.5, (250, 225, 190)), (1, (250, 225, 190))], v)
    img = sky.copy()
    for k, (base, col) in enumerate([(0.52, (226, 166, 108)), (0.6, (214, 146, 88)), (0.7, (198, 124, 70)), (0.82, (176, 104, 56))]):
        # smooth crests only: noise in the line made the light change from column to column
        x = np.linspace(0, 1, W)
        line = base + 0.05 * np.sin(x * (3 + k) + k * 1.7) + 0.015 * np.sin(x * (11 + 3 * k) + k)
        top = (line * H).astype(int)
        mask = yy >= top[None, :]
        # light from the left: the slope's side
        slope = np.gradient(line)[None, :] * W
        depth = np.clip((yy - top[None, :]) / (H * 0.25), 0, 1)
        # (the slope's light only near the crest, so it doesn't run down the dune in bands)
        light = 1 - 0.22 * np.tanh(slope * 4) * np.exp(-depth * 4)
        shade = np.array(col, np.float32) * (light * (1 - 0.3 * depth))[..., None]
        img[mask] = shade[mask]
    save(img, 'dunes.jpg')


def starry_night():
    img = gradient([(0, (6, 10, 30)), (0.6, (22, 32, 72)), (1, (40, 52, 96))], v)
    band = np.exp(-((v - (0.25 + 0.4 * u)) * 6) ** 2) * (0.6 + 0.4 * np.sin(u * 20 + v * 9))
    img = mix(img, (150, 140, 200), band * 0.35)
    stars = rng.random((H, W)) > 0.9985
    bright = rng.random((H, W)) ** 6
    star = Image.fromarray((stars * bright * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8))
    s = np.asarray(star, np.float32) / 255 * 3
    img = mix(img, (255, 250, 235), np.clip(s, 0, 1))
    line = 0.78 + 0.07 * ridge(W, smooth=0.9)
    img[yy >= (line * H)[None, :]] = (8, 10, 20)
    save(img, 'starry-night.jpg', blur=0)


def ocean_sunset():
    sky = gradient([(0, (60, 40, 110)), (0.3, (190, 80, 120)), (0.5, (250, 150, 90)), (0.56, (255, 200, 120))], v)
    sun = np.hypot((u - 0.5) * W, (v - 0.5) * H) < 70
    img = sky.copy()
    img[sun] = (255, 236, 190)
    glow = np.exp(-(np.hypot((u - 0.5) * W, (v - 0.5) * H) / 260) ** 2)
    img = mix(img, (255, 210, 150), glow * 0.6)
    horizon = int(0.56 * H)
    sea = gradient([(0.56, (210, 110, 100)), (0.7, (90, 50, 90)), (1, (25, 20, 50))], v)
    path = np.exp(-((u - 0.5) * (3 + 30 * (v - 0.56))) ** 2) * (0.5 + 0.5 * np.sin(yy * 0.9 + np.sin(xx * 0.05) * 3))
    sea = mix(sea, (255, 210, 150), path * 0.8)
    img[horizon:] = sea[horizon:]
    save(img, 'ocean-sunset.jpg')


def forest_mist():
    img = gradient([(0, (170, 200, 196)), (0.6, (214, 226, 214)), (1, (190, 205, 190))], v)
    for k, (base, col, size) in enumerate([(0.45, (130, 160, 150), 26), (0.55, (90, 124, 112), 34),
                                           (0.66, (56, 88, 76), 44), (0.8, (28, 52, 44), 58)]):
        ground = (base + 0.02 * ridge(W, octaves=3)) * H
        mask = yy >= ground[None, :]
        x = 0
        while x < W:
            h = size * rng.uniform(2.2, 3.6)
            w = size * rng.uniform(0.7, 1.0)
            gx = int(min(x, W - 1))
            top = ground[gx] - h
            tri = (yy >= top) & (yy <= ground[gx] + 4) & (np.abs(xx - x) <= (yy - top) / h * w)
            mask |= tri
            x += size * rng.uniform(0.6, 1.1)
        img[mask] = np.array(col, np.float32)
        img = mix(img, (214, 226, 214), np.full((H, W), 0.18 - 0.03 * k))
    save(img, 'forest-mist.jpg')


mountain_lake()
dunes()
starry_night()
ocean_sunset()
forest_mist()
