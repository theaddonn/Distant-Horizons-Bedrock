# Spike: live lightmap texture for DH_Terrain (static RE, no game launch)

All RVAs are image RVAs (Bedrock 1.26.51 Win x64). Note: `~/.cache/dh-re/strings_all.txt` addresses are **file offsets**,
not RVAs (add 0x1000 in .rdata): `xref.py off:0x...` is needed, a bare `xref.py ef45fa1` hits the wrong bytes.

## 1. Where the live lightmap lives

`mce::DynamicTexture` (uchar enum, 23 values) is a name table at .rdata `0x0E8E0438` (8-byte string pointers, indexed directly by
the enum value; a parallel length table is at `0x0E8E04F0`):

| key | name | key | name |
|---|---|---|---|
| 0..3 | `BrightnessTexture0..3` | 13..16 | `BiomeBlendingTexture0..3` |
| 4 | `SeasonFoliageTexture` | 17..20 | `Skin`, `LoadingScreen`, `LoadingScreenFont`, `Invalid` |
| 5..8 / 9..12 | `WeatherOcclusionTexture0..3` / `WeatherSnowOcclusionTexture0..3` | | |

(Index = key proven by `0x46EB180`: `movzx ecx,bl; shl ecx,3; lea rax,[0xE8E0438]; mov rax,[rcx+rax]`, `cmp bl,0x16 ; ja "Unknown dynamic texture"`.)
BrightnessTexture0..3 are the per-split-screen-client lightmaps (index = SubClientId, presumably; the callers pass a byte read from a
client-side object, see below). Header-wise they live in `MinecraftGameplayGraphicsResources::mDynamicTextures`
(`mcr::DynamicTextureResourceManager = cg::ResourceManager<std::shared_ptr<mce::ClientTexture>, mce::DynamicTexture, std::map>`).

**Accessor (get-or-create), RVA `0x046EB180`** (unique with the first 24 bytes of its prologue:
`41 57 41 56 41 54 56 57 55 53 48 81 EC 90 00 00 00 89 D3 48 89 CF 4C 8B B1 80 00 00 00`):

```
std::shared_ptr<mce::ClientTexture>&  MinecraftGameplayGraphicsResources::getDynamicTexture(DynamicTexture key)
   rcx = MinecraftGameplayGraphicsResources*   rdx(dl) = key        returns rax = &map_node->value
 046eb196  mov r14,[rcx+0x80]            ; std::map head node  -> mDynamicTextures is at +0x80
 046eb1fc  cmp [rdi+0x88],0x492492492492492   ; map size at +0x88 (max_size check on insert)
 046eb232  mov byte [rax+0x20], bl         ; node key (byte) at +0x20
 046eb269  mov rax,[rsi+0x28]  ... add rsi,0x28   ; node value (shared_ptr<ClientTexture>) at +0x28; returns &value
 046eb2b9  add rdi,0x90                    ; +0x90 = mTextureResourceService, passed to 0x4705de0 (create) on a miss
 046eb2c0..2fd  placeholder TextureDescription from .rdata 0xE8DDF00 (w=0x40,h=0x40,fmt=0x1c) + name from the table above
 046eb37f  mov [rdx+8],rcx ; mov [rdx+0x10],rax   ; fills ClientTexture{vptr@0, ResourceBlockTemplate* @8, ctrl @0x10}
```
(Offsets match the SDK header: `mDynamicTextures` = 64+8+16+8+16+8+8 = 0x80, then `mTextureResourceService` @0x90.)
Two near-identical clones exist (`0x0479AC80`, `0x0661BD80`, extra args) - ignore.

**Callers prove the chain to the object** (`0x0469A3CB`):
```
 0469a389  mov rax,[rbp+0x10] ; cmp byte [rax],0 ...   ; Bedrock::NonOwnerPointer<...> validity check (control block mIsValid)
 0469a3cb  mov rcx,[rbp+0x20]      ; NonOwnerPointer's pointee
 0469a3cf  add rcx,0x78            ; -> MinecraftGameplayGraphicsResources (= MinecraftGraphics::mMinecraftGameplayGraphicsResources)
 0469a3d3  mov edx,r15d            ; DynamicTexture key
 0469a3d6  call 0x46eb180
```
`0x78` agrees with the header: `EnableNonOwnerReferences`(24) + `AppPlatformListener`(32) + 8+8+24+8+16 = 0x78.

### Chain we can run
```
IClientInstance::getMinecraftGraphics()   (virtual, callable)  -> NotNullNonOwnerPtr<MinecraftGraphics>  (pointee @ +0x20 of the
                                            by-value wrapper in the caller above; use the SDK type's .get())
gfx  = MinecraftGraphics*  ;  res = (char*)gfx + 0x78            // MinecraftGameplayGraphicsResources
map  = res + 0x80   // std::map<uint8_t, shared_ptr<mce::ClientTexture>> (MSVC: head*@+0, size@+8); node: key@+0x20, value@+0x28
tex  = getDynamicTexture(res, subClientId /*0..3*/)   // RVA 0x46EB180, sig above; returns shared_ptr<ClientTexture>&
```
Type: `mce::ClientTexture` = `ClientResourcePointer<variant<monostate, mce::Texture, ClientResourcePointer<dragon::ResolvedTextureResource>>>`
(24 bytes: vptr + shared_ptr<ResourceBlockTemplate>). That is **exactly the `mce::ClientTexture` alternative of the renderMesh variant**:
`std::variant<...> v = *tex;` (copy-construct the ClientTexture alternative), or use the overload taking
`gsl::span<mce::ClientTexture const*>` (`Mesh::renderMesh(ctx, mat, span, start, count, OffscreenCaptureDescription const&, IndexBufferContainer const*)`),
which also lets us bind several textures at once; `_renderMesh` takes `static_vector<variant,8>` so slot 0 = lightmap, slot 1 = atlas also works.
Instead of calling `0x46EB180`, one may read the std::map directly (lookup key byte, node+0x28 -> shared_ptr), which avoids needing a sig
but never creates the entry. Calling it creates a *placeholder* 64x64 texture on a miss - only call after the engine has rendered a frame
(or guard: walk the map first and only use an existing node).

**Confidence on "this is the live lightmap": medium-high.** Evidence is circumstantial (names + sole consumer of the table is this
accessor + chunk pass below), not a traced `LightTexture::refresh` -> map write (LightTexture ctor/refresh have no symbols; I did not
locate them). Cheap runtime validation: log `tex->get()` (variant index; 2 = ResolvedTextureResource), and check that texture
dimensions/`isValid()`; the stuck-pink/black symptom disappears if right. If BrightnessTexture0 turns out to be a 64x64 placeholder
that is never updated, try the clones' other callers or `LightTexture` (its first 16-byte member `mUnk6fbdad` is plausibly the
shared_ptr/handle - unverified).

## 2. How the chunk pass binds `LightMapTexture`

- `RenderChunk.json` (material v26): `samplerDefinitionMap = {LightMapTexture reg 0, <sampler reg 1>, SeasonsTexture reg 2}`, all
  `Type2D`, `hasDefaultTexture=false`, `hasCustomTypeInfo=false`. Across all `~/.cache/dh-tools/unpacked/*/*.json` only 4 sampler entries
  carry those keys and **none** has either flag true. There is no material-level flag/default that auto-binds a named engine texture.
- Binding is hand-written in the render-chunk pass setup (huge function `0x05F06070-05F148C2`, ~58 KB, reached from `0x05E4B8FD`). It holds
  function-local statics `HashedString("LightMapTexture")` (string init at `0x05F1396D`, hash slot `.data 0x11E434A0`). The code linear-scans
  the *material's* sampler table (stride 0x30, compares FNV-1a hash at `entry->+0x28`), turns the hit into a sampler index
  and calls the frame-graph binder `0x0B7717F0` (`rcx` = pass builder `rbp+0xDE0`, `rdx` = {sampler idx u16, 1}, `r8` = material `+0x18`,
  `r9` = {u16 resource id loaded from a runtime-initialised static `.data 0x11D1CA2C`/`0x11D1CA28`, 0, flags}) (e.g. `0x05F06F81..06F7C`).
  So a material gets its lightmap only if (a) a sampler is literally named `LightMapTexture` **and** (b) the draw goes through that pass.
  Our out-of-band `renderMesh` path never runs it. **Not practical to piggy-back; do not pursue** (would need a hook inside a 58 KB function).
- `mBrightnessTex` (LevelRenderer) is dead because lightmaps moved to the `DynamicTexture` map above.

## 3. Fallback recipe (own 16x16 texture) - only if section 1 fails

MCAPI-callable pieces in the SDK:
- `mce::TexturePtr::TexturePtr(BedrockTexture const&, ResourceLocation const&, cg::TextureSetLayerType)` (also `(shared_ptr<TextureGroupBase>, ResourceLocation const&)`)
- `BedrockTexture::BedrockTexture()`, `bool BedrockTexture::load(mce::TextureResourceService&, mce::TextureContainer&&, IsMissingTexture, std::string_view debugName, cg::TextureSetLayerType)`,
  `bool BedrockTexture::_loadFromImageBuffer(cg::TextureSetImageContainer&, TextureSetLayerType, TextureResourceService&, TextureSetImageDescription const&, IsMissingTexture, string_view) const`, `void unload()`.
- `TextureResourceService` = `res + 0x90` (from section 1). `mce::TextureGroupBase::getTexture(ResourceLocation const&, bool, optional<uint>, TextureSetLayerType)` is a virtual (needs a group instance - none obtained yet).
- `BedrockTextureData::mClientTexture` is a `mce::ClientTexture` (24 B) so a `TexturePtr` built this way yields a `ClientTexture` too.
Update path: re-`load` a fresh `TextureContainer` (16x16 RGBA8) each time the CPU table changes (dirty-flag, a few times per second at most);
the `Update*TextureTransaction` types exist in headers but no MCAPI entry point to enqueue them was found.
Unknowns: `mce::TextureContainer` construction API (not inspected), `ResourceLocation` for a synthetic name, thread requirements (render thread).
Formula: **unknown** - `BaseLightTextureImageBuilder` subclasses are created by `LightTextureImageBuilderFactory::registerLightImageBuilders`
(lambdas 270-273 + custom-dimension variant, RTTI at `.data 0x11A8B7D0-0x11A8BA10`, per-dimension: overworld/nether/end/custom); the
vanilla Java shape is `block = pow-curve(blockLight/15)`, `sky = skyLight/15 * (daylight-dependent)`; do not trust it. The CPU fallback
already in the repo (`ConvertParams.cpuLighting`) is the safer fallback than reproducing the table.

## 4. Ranking

1. **Pass `*getDynamicTexture(gfxRes, 0)` (or SubClientId) as the `mce::ClientTexture` alternative** (res = `getMinecraftGraphics() + 0x78`, accessor RVA `0x46EB180`, or read the map at `res+0x80` directly). Confidence ~70% it is the live day/night 16x16 lightmap and fits the variant; ~90% on the offsets/ABI (two independent callers + header agree). Risks: the entry may be a stale 64x64 placeholder if some other object owns the updated one (check dimensions/`isValid`), key = SubClientId assumption, and `ResolvedTextureResource` lifetime across resource-pack reloads (re-fetch every frame, never cache).
2. **Keep the shipped CPU-lit DH_Terrain** (no texture; already working per Run 11). Zero risk; loses vanilla night/underground-dim parity only to the extent the CPU formula differs. Use as the automatic fallback if (1) returns null/invalid.
3. **Own 16x16 texture via `BedrockTexture::load(TextureResourceService&, TextureContainer&&, ...)`** with a CPU-computed table. Highest effort/uncertainty (TextureContainer API unexplored, per-update re-load, formula unknown); only worth it if (1) is invalid and CPU vertex lighting is visibly wrong.
Not recommended: hooking the chunk-pass binder (`0x05F06070`) or a material-level auto-bind (none exists).

## 5. Run 13 update (2026-10-19)

The dynamic `BrightnessTexture0` "sampling black" was not evidence against the texture: the LOD meshes had no uv0 vertex stream (MeshUploader seeded its template with `Tessellator::tex2`, which writes uv set 2), so every lookup sampled texel (0, 0) = block 0 / sky 0, which is black in any lightmap. With uv0 fixed the `dynamic` / `dynamic-swapped` modes (Numpad 3) are a valid test again: a correct lightmap gives daylight brightness on sky-lit surfaces; still black means the entry is the placeholder (or not bound); bright where it should be dark and vice versa means the axes are swapped. The default stays the procedural shader lightmap.
