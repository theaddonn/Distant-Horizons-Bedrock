/* fxc_lite.c - minimal fxc replacement: loads d3dcompiler_47.dll at runtime and calls D3DCompile.
 * Build:  x86_64-w64-mingw32-gcc -O2 -o fxc_lite.exe fxc_lite.c
 * Run:    WINEPREFIX=~/.local/share/amtool/wineprefix wine fxc_lite.exe in.hlsl entry profile out.bin
 * Under wine the DLL is wine's builtin d3dcompiler_47 (vkd3d-shader based, NOT Microsoft's fxc); on real
 * Windows it is Microsoft's compiler. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
typedef HRESULT (WINAPI *PFN)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, void**, void**);
typedef struct { void **vt; } Obj;
int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: fxc_lite in.hlsl entry profile out.bin\n"); return 2; }
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    if (!m) { fprintf(stderr, "no d3dcompiler_47\n"); return 3; }
    PFN f = (PFN)GetProcAddress(m, "D3DCompile");
    FILE *fp = fopen(argv[1], "rb"); if (!fp) return 4;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    char *src = malloc(n); fread(src, 1, n, fp); fclose(fp);
    Obj *code = 0, *err = 0;
    HRESULT hr = f(src, n, argv[1], 0, 0, argv[2], argv[3], 0, 0, (void**)&code, (void**)&err);
    if (err) { /* ID3DBlob: GetBufferPointer=slot3, GetBufferSize=slot4 */
        void *(WINAPI *gp)(void*) = err->vt[3]; SIZE_T (WINAPI *gs)(void*) = err->vt[4];
        fwrite(gp(err), 1, gs(err), stderr); }
    if (FAILED(hr) || !code) { fprintf(stderr, "D3DCompile failed 0x%lx\n", (unsigned long)hr); return 1; }
    void *(WINAPI *gp)(void*) = code->vt[3]; SIZE_T (WINAPI *gs)(void*) = code->vt[4];
    fp = fopen(argv[4], "wb"); fwrite(gp(code), 1, gs(code), fp); fclose(fp);
    printf("ok %s %s -> %s (%lu bytes)\n", argv[2], argv[3], argv[4], (unsigned long)gs(code));
    return 0;
}
