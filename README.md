# Distant Horizons (Bedrock)

A port of [Distant Horizons](https://gitlab.com/distant-horizons-team/distant-horizons) to Minecraft Bedrock Edition, as a [LeviLamina](https://github.com/LiteLDev/LeviLamina) client mod: level-of-detail rendering that draws terrain far beyond the normal render distance.

Work in progress.

## Compatibility

| Component | Version |
| --- | --- |
| Minecraft Bedrock for Windows | 1.26.51.01 |
| LeviLamina | 26.51.6, client variant |

## Building

```
xmake f -y -p windows -a x64 -m release
xmake
```

The built mod is in `bin/DistantHorizons`.

## License

LGPL-3.0, like the original mod. See `LICENSE` and `LICENSE.LESSER`.
