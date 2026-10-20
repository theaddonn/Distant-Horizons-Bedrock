# dist/

Prebuilt binary inputs that CI cannot produce. They are committed and shipped by `scripts/package.sh`.

## materials/DH_Terrain.material.bin

The custom RenderDragon material used for LOD terrain (4,408,148 bytes). It is derived from the vanilla
`MeshFallbackPosUVNormalColor` material with a custom vertex and fragment shader (Java-style near clip, far fog,
LOD lighting). `DH_Terrain.material.bin.sha256` holds its checksum; `scripts/package.sh` and the release workflow
verify it (`cd dist/materials && sha256sum -c DH_Terrain.material.bin.sha256`).

Why it is committed: building it needs `dxc`, the MaterialBinTool and the unpacked vanilla materials from the game
install (`~/.cache/dh-tools`), none of which exist on a CI runner.

### Rebuild (required whenever `tools/shaders/src/` or the material build scripts change)

```
bash tools/shaders/build_dh_terrain.sh        # offline; writes artifacts/shaders/B3/DH_Terrain.material.bin
cp artifacts/shaders/B3/DH_Terrain.material.bin dist/materials/
(cd dist/materials && sha256sum DH_Terrain.material.bin > DH_Terrain.material.bin.sha256)
```

The build is deterministic: a fresh build of the sources at the time of commit had the same sha256
(`dca16cee...1b72`) as the file verified in game.
