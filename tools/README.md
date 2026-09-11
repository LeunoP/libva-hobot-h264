# Verification and Benchmark Tools

This directory contains standalone test and benchmark tools for validating hardware zero-copy video playback pipelines on the D-Robotics RDK-X5 platform.

### 1. `test_directviv.c`
Validates that Vivante GC8000L proprietary OpenGL ES direct texture extension (`GL_VIV_direct_texture` / `glTexDirectVIVMap`) can bind contiguous NV12 physical memory allocated via Hobot ION memory manager directly to a `GL_TEXTURE_2D` texture.

### 2. `test_render_directviv.c`
Creates a `GL_VIV_NV12` direct texture and executes a complete GLES shader rendering pipeline to sample the NV12 texture into an RGB framebuffer, validating GPU hardware color space conversion.

### 3. `test_nv12_overlay.c`
Validates importing Hobot NV12 DMA-BUF into DRM KMS (`/dev/dri/card0`) using `drmPrimeFDToHandle` and `drmModeAddFB2` for hardware overlay planes.

### 4. `test_va_directviv_bench.c`
Full end-to-end benchmark pipeline:
1. Initializes VA-API with `libva-hobot` driver on `/dev/dri/card0`.
2. Hardware decodes high-bitrate H.264 video streams frame-by-frame via Wave521 VPU.
3. Exports each decoded surface's contiguous physical address via `vaExportSurfaceHandle`.
4. Maps physical addresses directly into Vivante GLES 2D textures using `glTexDirectVIVMap`.
5. Renders textured quads at up to 180+ FPS with zero CPU copying and zero frame drops.
