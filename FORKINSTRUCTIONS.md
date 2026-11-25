# Spherical Harmonics for Diffuse Lighting  
The Khronos **glTF-IBL-Sampler** now supports **Spherical Harmonics (SH9)** for diffuse lighting calculations. This implementation enables an accurate and efficient diffuse environment lighting by precomputing spherical harmonic coefficients from an environment map.  

The implementation can be found in:  

- **SH9.h / SH9.cpp** – These files contain the core logic for calculating the **9-band spherical harmonics (SH9)** coefficients.  

These coefficients are then passed to the fragment shader, where they are used to sample the diffuse lighting contribution from the environment cubemap.  

## B10G11R11_UFLOAT_PACK32 Format Support  
The sampler now supports output in the **B10G11R11_UFLOAT_PACK32** format, allowing for efficient storage and retrieval of high-precision lighting data.  

## Octahedral Radiance Map Output  
The sampler now supports an **octahedral mapped 2D** version of the filtered cube map via the **-outOctahedral** flag. The octahedral map packs the cubemap's mip chain into a single 2D KTX 1 texture (width = 2 x face size), sampled by the Lume runtime's octahedral indirect specular mode. The mapping is the exact inverse of the runtime's `GetOctahedralRadianceUv()`, with seam fixed border texels, so both representations sample identical radiance.

The implementation can be found in:  

- **octahedral.h / octahedral.cpp** – These files contain the core logic for converting the filtered R11G11B10 cubemap KTX 1 into its octahedral mapped 2D KTX 1.  

The cubemap must be saved as KTX 1 (a `.ktx` output) for the conversion, since the octahedral map is built from its texel data.  

## Environment Map Generation Tool  
A command-line tool is included in the **tools/** directory to generate glTF environments with references to the processed maps. The tool processes an HDR environment map and produces a specular map, a skybox map, and the precomputed spherical harmonic coefficients.  

The specular and skybox maps are derived from the same cubemap, but the tool allows specifying different resolutions for each.

The **glTF-IBL-Sampler** generates `.ktx2` files by default. To ensure broader compatibility, the tool also converts these to `.ktx`. Both formats are stored in the `maps/processed/images/` directory.  



### Usage Example  
```sh
./generateEnvironment.bat quarry_02_2k.hdr 256 512
