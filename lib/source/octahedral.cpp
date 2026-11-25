#include "octahedral.h"

#include "FileHelper.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace IBLLib
{
	namespace
	{
		// Only the layout this sampler writes is handled: little endian, uncompressed, 6 faces
		// (cubemap), R11G11B10 float texels, full mip chain, no key/value data.

		constexpr uint8_t KTX_MAGIC[12] = { 0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n' };
		constexpr uint32_t KTX_ENDIANNESS = 0x04030201u;
		constexpr uint32_t GL_UNSIGNED_INT_10F_11F_11F_REV = 0x8C3Bu;
		constexpr uint32_t GL_R11F_G11F_B10F = 0x8C3Au;
		constexpr uint32_t GL_RGB = 0x1907u;
		constexpr size_t KTX_HEADER_SIZE = 64u; // 12 magic bytes + 12 uint32 fields
		constexpr size_t KTX_LEVEL_HEADER_SIZE = 4u;

		uint32_t readU32(const std::vector<char>& _buffer, size_t _offset)
		{
			const uint8_t* data = reinterpret_cast<const uint8_t*>(_buffer.data()) + _offset;
			return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
				(static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
		}

		void writeU32(std::vector<uint8_t>& _out, uint32_t _value)
		{
			_out.push_back(static_cast<uint8_t>(_value & 0xFFu));
			_out.push_back(static_cast<uint8_t>((_value >> 8u) & 0xFFu));
			_out.push_back(static_cast<uint8_t>((_value >> 16u) & 0xFFu));
			_out.push_back(static_cast<uint8_t>((_value >> 24u) & 0xFFu));
		}

		// One mip level of the source cubemap: six faces of R11G11B10 packed texels in the KTX face
		// order (+X, -X, +Y, -Y, +Z, -Z).
		struct CubemapLevel
		{
			uint32_t faceSize = 0; // width == height of a face
			const uint32_t* faces[6] = {};
		};

		struct Vec2
		{
			float x = 0.0f;
			float y = 0.0f;
		};

		struct Vec3
		{
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
		};

		// Direction -> cubemap face and texel coordinates, following the runtime's samplerCube face
		// selection and uv convention (Vulkan spec "Cube map face selection") so a texel of the
		// octahedral map holds exactly the cubemap content the runtime samples for the same direction.
		Vec2 directionToCubemapUv(const Vec3 _dir, size_t& _face)
		{
			const float absX = std::fabs(_dir.x);
			const float absY = std::fabs(_dir.y);
			const float absZ = std::fabs(_dir.z);
			float maxAxis = (absX > absY) ? absX : absY;
			maxAxis = (absZ > maxAxis) ? absZ : maxAxis;

			Vec2 uv;
			if (maxAxis == absX)
			{
				_face = _dir.x > 0.0f ? 0u : 1u;
				uv = _dir.x > 0.0f ? Vec2{-_dir.z, -_dir.y} : Vec2{_dir.z, -_dir.y};
			}
			else if (maxAxis == absY)
			{
				_face = _dir.y > 0.0f ? 2u : 3u;
				uv = _dir.y > 0.0f ? Vec2{_dir.x, _dir.z} : Vec2{_dir.x, -_dir.z};
			}
			else
			{
				_face = _dir.z > 0.0f ? 4u : 5u;
				uv = _dir.z > 0.0f ? Vec2{_dir.x, -_dir.y} : Vec2{-_dir.x, -_dir.y};
			}
			return Vec2{0.5f * uv.x / maxAxis + 0.5f, 0.5f * uv.y / maxAxis + 0.5f};
		}

		// Inverse of the runtime's GetOctahedralRadianceUv(): octahedral map coordinates in [-1,1]
		// mapped back to the environment direction they store.
		Vec3 octahedralCoordsToDirection(const float _ex, const float _ey)
		{
			const float dirY = 1.0f - std::fabs(_ex) - std::fabs(_ey);
			float dx = _ex;
			float dz = _ey;
			if (dirY < 0.0f)
			{
				// lower hemisphere: the runtime folds (1 - abs(zx)) * signNotZero(xz) into the corners,
				// undo exactly that fold (dirY keeps the negative hemisphere value)
				const float signX = (dx >= 0.0f) ? 1.0f : -1.0f;
				const float signZ = (dz >= 0.0f) ? 1.0f : -1.0f;
				dx = (1.0f - std::fabs(_ey)) * signX;
				dz = (1.0f - std::fabs(_ex)) * signZ;
			}
			const float length = std::sqrt(dx * dx + dirY * dirY + dz * dz);
			const float invLength = length > 0.0f ? 1.0f / length : 0.0f;
			return Vec3{dx * invLength, dirY * invLength, dz * invLength};
		}

		Vec2 octahedralTexelCenterCoords(const uint32_t _x, const uint32_t _y, const uint32_t _size)
		{
			return Vec2{(_x + 0.5f) / _size * 2.0f - 1.0f, (_y + 0.5f) / _size * 2.0f - 1.0f};
		}

		// Seam fix for the straight borders of the map (the four cut lines of the lower-hemisphere fold).
		// The runtime samples with bilinear filtering and clamp-to-edge addressing, and the UVs of
		// directions on opposite sides of a cut line jump between opposite stretches of the border ring
		// (a footprint never crosses the border in UV space). With plain center-direction content those
		// footprints read texels whose directions are displaced to either side of the cut line, so the
		// sampled radiance steps as a reflection vector sweeps across the cut - a hard seam, strongest
		// where the environment has sharp features (the sun) in the lower hemisphere.
		//
		// Fix: a texel of the outermost ring that lies outside the diamond stores the radiance of the
		// cut line the ring sits on - its border coordinate pinned to the map edge (-1/+1). Both sides
		// of a border then store the identical cut-line radiance (the four corner texels all store the
		// -y pole), so the clamped footprints converge to the same value from either side and the seam
		// disappears. Ring texels on the diamond edge are the equator crossings of the cut lines; they
		// are pinned as well (their footprints serve samples on both sides of the cut just below the
		// equator, and the two texels of a crossing hold the identical cut-line direction). Ring texels
		// strictly inside the diamond keep their center-direction content.
		void seamFixedTexelCoords(const uint32_t _x, const uint32_t _y, const uint32_t _size, Vec2& _coords)
		{
			const bool borderX = (_x == 0u || _x == _size - 1u);
			const bool borderY = (_y == 0u || _y == _size - 1u);
			if (!borderX && !borderY)
			{
				return;
			}
			if (std::fabs(_coords.x) + std::fabs(_coords.y) < 1.0f)
			{
				return;
			}
			if (borderX)
			{
				_coords.x = (_x == 0u) ? -1.0f : 1.0f;
			}
			if (borderY)
			{
				_coords.y = (_y == 0u) ? -1.0f : 1.0f;
			}
		}

		uint32_t sampleCubemap(const CubemapLevel& _level, const Vec3 _dir)
		{
			size_t face = 0;
			const Vec2 uv = directionToCubemapUv(_dir, face);
			const uint32_t faceSize = _level.faceSize;
			int x = static_cast<int>(uv.x * faceSize);
			int y = static_cast<int>(uv.y * faceSize);
			x = (x < 0) ? 0 : ((x >= static_cast<int>(faceSize)) ? static_cast<int>(faceSize) - 1 : x);
			y = (y < 0) ? 0 : ((y >= static_cast<int>(faceSize)) ? static_cast<int>(faceSize) - 1 : y);
			return _level.faces[face][static_cast<size_t>(y) * faceSize + static_cast<size_t>(x)];
		}
	} // namespace

	Result bakeOctahedralMap(const char* _cubemapPath, const char* _outputPath)
	{
		std::vector<char> cubemap;
		if (!readFile(_cubemapPath, cubemap) || cubemap.size() < KTX_HEADER_SIZE)
		{
			printf("Could not read the cubemap ktx file at %s \n", _cubemapPath);
			return Result::FileNotFound;
		}

		if (std::memcmp(cubemap.data(), KTX_MAGIC, sizeof(KTX_MAGIC)) != 0)
		{
			printf("The cubemap at %s is not a KTX file \n", _cubemapPath);
			return Result::KtxError;
		}

		const uint32_t endianness = readU32(cubemap, 12u);
		const uint32_t glType = readU32(cubemap, 16u);
		const uint32_t glTypeSize = readU32(cubemap, 20u);
		const uint32_t glInternalFormat = readU32(cubemap, 28u);
		const uint32_t pixelWidth = readU32(cubemap, 36u);
		const uint32_t pixelHeight = readU32(cubemap, 40u);
		const uint32_t numberOfFaces = readU32(cubemap, 52u);
		const uint32_t numberOfMipmapLevels = readU32(cubemap, 56u);
		const uint32_t bytesOfKeyValueData = readU32(cubemap, 60u);

		if (endianness != KTX_ENDIANNESS)
		{
			printf("Unsupported cubemap KTX byte order \n");
			return Result::KtxError;
		}
		if (glType != GL_UNSIGNED_INT_10F_11F_11F_REV || glTypeSize != 4u || glInternalFormat != GL_R11F_G11F_B10F)
		{
			printf("Unsupported cubemap KTX format (expected R11G11B10 float) \n");
			return Result::KtxError;
		}
		if (numberOfFaces != 6u || pixelWidth != pixelHeight || pixelWidth < 2u || numberOfMipmapLevels < 1u)
		{
			printf("Unsupported cubemap KTX layout (expected a mipmapped square cubemap) \n");
			return Result::KtxError;
		}
		if (numberOfMipmapLevels > 16u)
		{
			printf("Unsupported cubemap KTX mip count \n");
			return Result::KtxError;
		}

		// Locate the mip levels: per level a uint32 image size followed by the six faces (each face
		// image is padded to four byte multiples, which the R11G11B10 texel size already satisfies).
		const size_t keyValueDataOffset = KTX_HEADER_SIZE + bytesOfKeyValueData;
		std::vector<CubemapLevel> levels;
		levels.reserve(numberOfMipmapLevels);
		size_t offset = keyValueDataOffset;
		for (uint32_t level = 0u; level < numberOfMipmapLevels; ++level)
		{
			if (offset + KTX_LEVEL_HEADER_SIZE > cubemap.size())
			{
				printf("The cubemap KTX is truncated \n");
				return Result::KtxError;
			}
			const uint32_t imageSize = readU32(cubemap, offset);
			offset += KTX_LEVEL_HEADER_SIZE;
			const uint32_t faceSize = (pixelWidth >> level) > 0u ? (pixelWidth >> level) : 1u;
			const size_t faceBytes = static_cast<size_t>(faceSize) * faceSize * 4u;
			const size_t paddedFaceBytes = (faceBytes + 3u) & ~size_t(3u);
			if (imageSize != faceBytes || offset + paddedFaceBytes * 6u > cubemap.size())
			{
				printf("The cubemap KTX mip data is truncated or corrupt \n");
				return Result::KtxError;
			}

			CubemapLevel out;
			out.faceSize = faceSize;
			for (size_t face = 0u; face < 6u; ++face)
			{
				out.faces[face] =
					reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(cubemap.data()) + offset);
				offset += paddedFaceBytes;
			}
			levels.push_back(out);
		}

		// Bake the octahedral map: width = 2 x cubemap face size, same mip count as the source.
		const uint32_t octaWidth = pixelWidth * 2u;

		std::vector<uint8_t> out;
		out.reserve(offset - keyValueDataOffset + KTX_HEADER_SIZE);

		// Header. 2D image, single face, same format and mip count as the source cubemap.
		const uint8_t magicSize = static_cast<uint8_t>(sizeof(KTX_MAGIC));
		for (uint8_t i = 0; i < magicSize; ++i)
		{
			out.push_back(KTX_MAGIC[i]);
		}
		writeU32(out, KTX_ENDIANNESS);
		writeU32(out, glType);
		writeU32(out, glTypeSize);
		writeU32(out, GL_RGB);
		writeU32(out, glInternalFormat);
		writeU32(out, GL_RGB);
		writeU32(out, octaWidth);
		writeU32(out, octaWidth);
		writeU32(out, 0u); // pixelDepth
		writeU32(out, 0u); // numberOfArrayElements
		writeU32(out, 1u); // numberOfFaces
		writeU32(out, numberOfMipmapLevels);
		writeU32(out, 0u); // bytesOfKeyValueData

		for (uint32_t level = 0u; level < numberOfMipmapLevels; ++level)
		{
			const CubemapLevel& source = levels[level];
			const uint32_t octaSize = (octaWidth >> level) > 0u ? (octaWidth >> level) : 1u;
			const size_t imageBytes = static_cast<size_t>(octaSize) * octaSize * 4u;
			writeU32(out, static_cast<uint32_t>(imageBytes));

			const size_t levelOffset = out.size();
			out.resize(out.size() + imageBytes);
			auto* texels = reinterpret_cast<uint32_t*>(out.data() + levelOffset);
			for (uint32_t y = 0u; y < octaSize; ++y)
			{
				for (uint32_t x = 0u; x < octaSize; ++x)
				{
					Vec2 coords = octahedralTexelCenterCoords(x, y, octaSize);
					seamFixedTexelCoords(x, y, octaSize, coords);
					texels[static_cast<size_t>(y) * octaSize + x] =
						sampleCubemap(source, octahedralCoordsToDirection(coords.x, coords.y));
				}
			}
		}

		if (!writeFile(_outputPath, out))
		{
			printf("Could not save the octahedral map to path %s \n", _outputPath);
			return Result::KtxError;
		}

		return Result::Success;
	}
} // !IBLLib
