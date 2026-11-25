#pragma once

#include "ResultType.h"

namespace IBLLib
{
	// Convert a filtered R11G11B10 cubemap KTX 1 into its octahedral mapped 2D KTX 1 (same mip
	// chain, width = 2 x face size). The mapping is the exact inverse of the Lume runtime's
	// GetOctahedralRadianceUv(), with seam fixed border texels, so the runtime samples for a
	// direction exactly the cubemap texel this map stores.
	Result bakeOctahedralMap(const char* _cubemapPath, const char* _outputPath);
} // !IBLLib
