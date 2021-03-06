/*! \file */ 
#ifndef _SHP_MEM_TILES
#define _SHP_MEM_TILES

#include "tile_data.h"

class ShpMemTiles : public TileDataSource
{
public:
	ShpMemTiles(OSMStore &osmStore, uint baseZoom);

	// Find intersecting shapefile layer
	std::vector<std::string> FindIntersecting(const std::string &layerName, Box &box) const;
	bool Intersects(const std::string &layerName, Box &box) const;
	void CreateNamedLayerIndex(const std::string &layerName);

	// Used in shape file loading
	OutputObjectRef AddObject(uint_least8_t layerNum,
		const std::string &layerName, 
		enum OutputGeometryType geomType,
		Geometry geometry, 
		bool isIndexed, bool hasName, const std::string &name, AttributeStoreRef attributes);

	void AddObject(TileCoordinates const &index, OutputObjectRef const &oo) {
		tileIndex[index].push_back(oo);
	}
private:
	std::vector<uint> findIntersectingGeometries(const std::string &layerName, Box &box) const;
	std::vector<uint> verifyIntersectResults(std::vector<IndexValue> &results, Point &p1, Point &p2) const;
	std::vector<std::string> namesOfGeometries(std::vector<uint> &ids) const;

	/// Add an OutputObject to all tiles between min/max lat/lon
	void addToTileIndexByBbox(OutputObjectRef &oo, 
		double minLon, double minLatp, double maxLon, double maxLatp);

	/// Add an OutputObject to all tiles along a polyline
	void addToTileIndexPolyline(OutputObjectRef &oo, Geometry *geom);

	OSMStore &osmStore;

	std::vector<OutputObjectRef> cachedGeometries;					// prepared boost::geometry objects (from shapefiles)
	std::map<uint, std::string> cachedGeometryNames;			//  | optional names for each one
	std::map<std::string, RTree> indices;			// Spatial indices, boost::geometry::index objects for shapefile indices
};

#endif //_OSM_MEM_TILES

