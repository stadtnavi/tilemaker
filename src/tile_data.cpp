#include <algorithm>
#include <iostream>
#include "tile_data.h"
using namespace std;

typedef std::pair<OutputObjectsConstIt,OutputObjectsConstIt> OutputObjectsConstItPair;

void MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileCoordinatesSet &dstCoords) {
	if (zoom==baseZoom) {
		// at z14, we can just use tileIndex
		for (auto it = srcTiles.begin(); it!= srcTiles.end(); ++it) {
			TileCoordinates index = it->first;
			dstCoords.insert(index);
		}
	} else {
		// otherwise, we need to run through the z14 list, and assign each way
		// to a tile at our zoom level
		for (auto it = srcTiles.begin(); it!= srcTiles.end(); ++it) {
			TileCoordinates index = it->first;
			TileCoordinate tilex = index.x / pow(2, baseZoom-zoom);
			TileCoordinate tiley = index.y / pow(2, baseZoom-zoom);
			TileCoordinates newIndex(tilex, tiley);
			dstCoords.insert(newIndex);
		}
	}
}

void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, 
	std::vector<OutputObjectRef> &dstTile) {
	if (zoom==baseZoom) {
		// at z14, we can just use tileIndex
		auto oosetIt = srcTiles.find(dstIndex);
		if(oosetIt == srcTiles.end()) return;
		dstTile.insert(dstTile.end(), oosetIt->second.begin(), oosetIt->second.end());
	} else {
		// otherwise, we need to run through the z14 list, and assign each way
		// to a tile at our zoom level
		int scale = pow(2, baseZoom-zoom);
		TileCoordinates srcIndex1(dstIndex.x*scale, dstIndex.y*scale);
		TileCoordinates srcIndex2((dstIndex.x+1)*scale, (dstIndex.y+1)*scale);

		for(int x=srcIndex1.x; x<srcIndex2.x; x++)
		{
			for(int y=srcIndex1.y; y<srcIndex2.y; y++)
			{
				TileCoordinates srcIndex(x, y);
				auto oosetIt = srcTiles.find(srcIndex);
				if(oosetIt == srcTiles.end()) continue;
				for (auto it = oosetIt->second.begin(); it != oosetIt->second.end(); ++it) {
					OutputObjectRef oo = *it;
					if (oo->minZoom > zoom) continue;
					dstTile.insert(dstTile.end(), oo);
				}
			}
		}
	}
}

// *********************************

ObjectsAtSubLayerIterator::ObjectsAtSubLayerIterator(OutputObjectsConstIt it, const class TileData &tileData):
	OutputObjectsConstIt(it),
	tileData(tileData) { }

// ********************************

TilesAtZoomIterator::TilesAtZoomIterator(TileCoordinatesSet::const_iterator it, class TileData &tileData, uint zoom):
	TileCoordinatesSet::const_iterator(it),
	tileData(tileData),
	zoom(zoom) {

	RefreshData();
}

TileCoordinates TilesAtZoomIterator::GetCoordinates() const {
	TileCoordinatesSet::const_iterator it = *this;
	return *it;
}

ObjectsAtSubLayerConstItPair TilesAtZoomIterator::GetObjectsAtSubLayer(uint_least8_t layerNum) const {
	TileCoordinatesSet::const_iterator it = *this;

    struct layerComp
    {
        bool operator() ( const OutputObjectRef &x, uint_least8_t layer ) const { return x->layer < layer; }
        bool operator() ( uint_least8_t layer, const OutputObjectRef &x ) const { return layer < x->layer; }
    };

	// compare only by `layer`
	// We get the range within ooList, where the layer of each object is `layerNum`.
	// Note that ooList is sorted by a lexicographic order, `layer` being the most significant.
	const std::vector<OutputObjectRef> &ooList = data;

	OutputObjectsConstItPair ooListSameLayer = equal_range(ooList.begin(), ooList.end(), layerNum, layerComp());
	return ObjectsAtSubLayerConstItPair(ObjectsAtSubLayerIterator(ooListSameLayer.first, tileData), ObjectsAtSubLayerIterator(ooListSameLayer.second, tileData));
}

TilesAtZoomIterator& TilesAtZoomIterator::operator++() {
	TileCoordinatesSet::const_iterator::operator++();
	RefreshData();
	return *this;
}

TilesAtZoomIterator TilesAtZoomIterator::operator++(int a) {
	TileCoordinatesSet::const_iterator::operator++(a);
	RefreshData();
	return *this;
}

TilesAtZoomIterator& TilesAtZoomIterator::operator--() {
	TileCoordinatesSet::const_iterator::operator--();
	RefreshData();
	return *this;
}

TilesAtZoomIterator TilesAtZoomIterator::operator--(int a) {
	TileCoordinatesSet::const_iterator::operator--(a);
	RefreshData();
	return *this;
}

void TilesAtZoomIterator::RefreshData() {
	data.clear();
	TileCoordinatesSet::const_iterator it = *this;
	if(it == tileData.tileCoordinates.end()) return;

	for(size_t i=0; i<tileData.sources.size(); i++)
		tileData.sources[i]->MergeSingleTileDataAtZoom(*it, zoom, data);

	sort(data.begin(), data.end());
	data.erase(unique(data.begin(), data.end()), data.end());
}

// *********************************

TileData::TileData(std::vector<class TileDataSource *> const &sources, uint zoom):
	sources(sources),
	zoom(zoom) {

	// Create list of tiles
	tileCoordinates.clear();
	for(size_t i=0; i<sources.size(); i++)
		sources[i]->MergeTileCoordsAtZoom(zoom, tileCoordinates);
}

class TilesAtZoomIterator TileData::GetTilesAtZoomBegin() {
	return TilesAtZoomIterator(tileCoordinates.begin(), *this, zoom);
}

class TilesAtZoomIterator TileData::GetTilesAtZoomEnd() {
	return TilesAtZoomIterator(tileCoordinates.end(), *this, zoom);
}

size_t TileData::GetTilesAtZoomSize() const {
	return tileCoordinates.size();
}


