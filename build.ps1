# ============================================================
#  build.ps1 - cs2_cheat
#  Compiler : zig c++ (Clang 18, portable)
#  Cara     : .\build.ps1          (Release)
#             .\build.ps1 debug    (Debug)
#             .\build.ps1 clean    (Hapus exe)
# ============================================================
param(
    [string]$Mode = ""
)

$ErrorActionPreference = "SilentlyContinue"

$ZIG_DIR = "$PSScriptRoot\zig-windows-x86_64-0.13.0"
$ZIG     = "$ZIG_DIR\zig.exe"
$ROOT    = $PSScriptRoot
$OUT     = "$ROOT\cs2_cheat.exe"

# ---- Clean -----------------------------------------------------------------
if ($Mode -eq "clean") {
    if (Test-Path $OUT) {
        Remove-Item $OUT -Force
        Write-Host "[+] Hapus $OUT"
    }
    Write-Host "[+] Clean selesai."
    exit 0
}

# ---- Argumen ---------------------------------------------------------------
$OPT_LEVEL = @("-O2")
$EXTRA_DEFINES = @()

if ($Mode -eq "debug") {
    $OPT_LEVEL = @("-O0", "-g")
}

# ---- Verifikasi zig --------------------------------------------------------
if (-not (Test-Path $ZIG)) {
    Write-Host "[-] zig.exe tidak ditemukan di: $ZIG"
    exit 1
}

$ZIG_VER = & $ZIG version 2>$null
Write-Host "[+] Zig/Clang : $ZIG_VER"
Write-Host "[+] Mode      : $Mode"
Write-Host "[+] Output    : $OUT"
Write-Host ""

# ---- Source files ----------------------------------------------------------
$SOURCES = @(
    "$ROOT\cheat\main.cpp",
    "$ROOT\cheat\overlay.cpp",
    "$ROOT\math\vec2.cpp",
    "$ROOT\math\vec3.cpp",
    "$ROOT\math\mat3x4.cpp",
    "$ROOT\math\mat4x4.cpp",
    "$ROOT\third_party\imgui\imgui.cpp",
    "$ROOT\third_party\imgui\imgui_draw.cpp",
    "$ROOT\third_party\imgui\imgui_widgets.cpp",
    "$ROOT\third_party\imgui\imgui_tables.cpp",
    "$ROOT\third_party\imgui\imgui_impl_win32.cpp",
    "$ROOT\third_party\imgui\imgui_impl_dx11.cpp"
)

# ---- Include dirs ----------------------------------------------------------
$INCLUDES = @(
    "-I$ROOT",
    "-I$ROOT\cheat",
    "-I$ROOT\third_party\imgui"
)

# ---- Compile flags ---------------------------------------------------------
$FLAGS = @(
    "-std=c++23",
    "-target", "x86_64-windows-gnu",
    "-msse4.1", "-mavx2", "-mno-avx512f"
) + $OPT_LEVEL + @(
    "-DNDEBUG",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-ignored-attributes"
) + $EXTRA_DEFINES + @(
    "-Wl,--subsystem,windows"
)

# ---- Linker flags ----------------------------------------------------------
$LIBS = @(
    "-lkernel32", "-luser32", "-lntdll", "-lshell32",
    "-ld3d11", "-ld3dcompiler_47", "-ldxgi", "-ldcomp", "-ldwmapi", "-lgdi32", "-lwinmm"
)

# ---- Compile manifest.rc -> manifest.res (UAC requireAdministrator) --------
$MANIFEST_RC  = "$ROOT\cheat\manifest.rc"
$MANIFEST_RES = "$ROOT\cheat\manifest.res"
if (-not (Test-Path $MANIFEST_RES) -or
    (Get-Item $MANIFEST_RES).LastWriteTime -lt (Get-Item $MANIFEST_RC).LastWriteTime) {
    Write-Host "[*] Compile manifest.rc..."
    & $ZIG rc $MANIFEST_RC $MANIFEST_RES 2>&1 | Out-Null
}
if (Test-Path $MANIFEST_RES) {
    $SOURCES += $MANIFEST_RES
    Write-Host "[+] Manifest UAC disertakan (requireAdministrator)"
} else {
    Write-Host "[!] Manifest tidak dikompile - program mungkin perlu dijalankan manual sebagai Admin"
}

# ---- Hapus exe lama agar bisa deteksi kegagalan ----------------------------
# [FIX] Exe yang masih berjalan terkunci: Remove-Item gagal senyap lalu
# Test-Path masih melihat exe LAMA -> "Build berhasil" palsu.
$BUILD_START = Get-Date
if (Test-Path $OUT) {
    Remove-Item $OUT -Force
    if (Test-Path $OUT) {
        Write-Host "[-] EXE LAMA TERKUNCI - cs2_cheat.exe masih berjalan!" -ForegroundColor Red
        Write-Host "    Tutup dulu (SHIFT+END di game / tombol Exit di menu), lalu build ulang."
        exit 1
    }
}

# ---- Compile + Link --------------------------------------------------------
Write-Host "[*] Compile..."

$args_all = @("c++") + $FLAGS + $INCLUDES + $SOURCES + @("-o", $OUT) + $LIBS

& $ZIG @args_all 2>&1 | ForEach-Object {
    if ($_ -match "error:") { Write-Host $_ -ForegroundColor Red }
}

# ---- Check result ----------------------------------------------------------
# [FIX] Harus exe BARU (waktu tulis > mulai build), bukan sekadar "ada exe".
$exe = Get-Item $OUT -ErrorAction SilentlyContinue
if ($exe -and $exe.LastWriteTime -gt $BUILD_START) {
    Write-Host ""
    Write-Host "[+] Build berhasil!" -ForegroundColor Green
    Write-Host ("[+] Output : {0} ({1} bytes, {2})" -f $OUT, $exe.Length, $exe.LastWriteTime.ToString('HH:mm:ss'))
    exit 0
} else {
    Write-Host ""
    Write-Host "[-] Build gagal (exe tidak terganti - cek error compile di atas)!" -ForegroundColor Red
    exit 1
}
