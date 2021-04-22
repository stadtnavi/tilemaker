/*! \file */ 

// C++ includes
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <chrono>

// Other utilities
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/variant.hpp>
#include <boost/algorithm/string.hpp>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"

#ifndef _MSC_VER
#include <sys/resource.h>
#endif

#include "geomtypes.h"

// Tilemaker code
#include "helpers.h"
#include "coordinates.h"

#include "attribute_store.h"
#include "output_object.h"
#include "osm_lua_processing.h"
#include "mbtiles.h"
#include "write_geometry.h"

#include "shared_data.h"
#include "read_pbf.h"
#include "read_shp.h"
#include "tile_worker.h"
#include "osm_mem_tiles.h"
#include "shp_mem_tiles.h"

#include <boost/asio/post.hpp>
#include <boost/interprocess/streams/bufferstream.hpp>

// Namespaces
using namespace std;
namespace po = boost::program_options;
namespace geom = boost::geometry;

// Global verbose switch
bool verbose = false;

void WriteSqliteMetadata(rapidjson::Document const &jsonConfig, SharedData &sharedData, LayerDefinition const &layers)
{
	// Write mbtiles 1.3+ json object
	sharedData.mbtiles.writeMetadata("json", layers.serialiseToJSON());

	// Write user-defined metadata
	if (jsonConfig["settings"].HasMember("metadata")) {
		const rapidjson::Value &md = jsonConfig["settings"]["metadata"];
		for(rapidjson::Value::ConstMemberIterator it=md.MemberBegin(); it != md.MemberEnd(); ++it) {
			if (it->value.IsString()) {
				sharedData.mbtiles.writeMetadata(it->name.GetString(), it->value.GetString());
			} else {
				rapidjson::StringBuffer strbuf;
				rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
				it->value.Accept(writer);
				sharedData.mbtiles.writeMetadata(it->name.GetString(), strbuf.GetString());
			}
		}
	}
	sharedData.mbtiles.closeForWriting();
}

void WriteFileMetadata(rapidjson::Document const &jsonConfig, SharedData const &sharedData, LayerDefinition const &layers)
{
	if(sharedData.config.compress) 
		std::cout << "When serving compressed tiles, make sure to include 'Content-Encoding: gzip' in your webserver configuration for serving pbf files"  << std::endl;

	rapidjson::Document document;
	document.SetObject();

	if (jsonConfig["settings"].HasMember("filemetadata")) {
		const rapidjson::Value &md = jsonConfig["settings"]["filemetadata"];
		document.CopyFrom(md, document.GetAllocator());
	}

	rapidjson::Value boundsArray(rapidjson::kArrayType);
	boundsArray.PushBack(rapidjson::Value(sharedData.config.minLon), document.GetAllocator());
	boundsArray.PushBack(rapidjson::Value(sharedData.config.minLat), document.GetAllocator());
	boundsArray.PushBack(rapidjson::Value(sharedData.config.maxLon), document.GetAllocator());
	boundsArray.PushBack(rapidjson::Value(sharedData.config.maxLat), document.GetAllocator());
	document.AddMember("bounds", boundsArray, document.GetAllocator());

	document.AddMember("name", rapidjson::Value().SetString(sharedData.config.projectName.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("version", rapidjson::Value().SetString(sharedData.config.projectVersion.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("description", rapidjson::Value().SetString(sharedData.config.projectDesc.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("minzoom", rapidjson::Value(sharedData.config.startZoom), document.GetAllocator());
	document.AddMember("maxzoom", rapidjson::Value(sharedData.config.endZoom), document.GetAllocator());
	document.AddMember("vector_layers", layers.serialiseToJSONValue(document.GetAllocator()), document.GetAllocator());

	auto fp = std::fopen((sharedData.outputFile + "/metadata.json").c_str(), "w");

	char writeBuffer[65536];
	rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
	rapidjson::Writer<rapidjson::FileWriteStream> writer(os);
	document.Accept(writer);

	fclose(fp);
}

void generate_from_index(OSMStore &osmStore, PbfReaderOutput *output)
{
	PbfReaderOutput::tag_map_t currentTags;

	std::cout << "Generate from index file" << std::endl;
	for(std::size_t i = 0; i < osmStore.total_pbf_node_entries(); ++i) {
		if((i + 1) % 10000 == 0) {
			cout << "Generating node " << (i + 1) << " / " << osmStore.total_pbf_node_entries() << "        \r";
			cout.flush();
		}
		auto const &entry = osmStore.pbf_node_entry(i);

		currentTags.clear();
		for(auto const &i: entry.tags) {
			currentTags.emplace(std::piecewise_construct,
				std::forward_as_tuple(i.first.begin(), i.first.end()), 
				std::forward_as_tuple(i.second.begin(), i.second.end()));
		}

		output->setNode(entry.nodeId, entry.node, currentTags);
	}

	for(std::size_t i = 0; i < osmStore.total_pbf_way_entries(); ++i) {
		if((i + 1) % 10000 == 0) {
			cout << "Generating way " << (i + 1) << " / " <<  osmStore.total_pbf_way_entries()<< "        \r";
			cout.flush();
		}

		auto const &entry = osmStore.pbf_way_entry(i);

		currentTags.clear();
		for(auto const &i: entry.tags) {
			currentTags.emplace(std::piecewise_construct,
				std::forward_as_tuple(i.first.begin(), i.first.end()), 
				std::forward_as_tuple(i.second.begin(), i.second.end()));
		}

		output->setWay(entry.wayId, entry.nodeVecHandle, currentTags);
	}

	for(std::size_t i = 0; i < osmStore.total_pbf_relation_entries(); ++i) {
		if((i + 1) == osmStore.total_pbf_relation_entries() || ((i + 1) % 100 == 0)) {
			cout << "Generating relation " << (i + 1) << " / " << osmStore.total_pbf_relation_entries() << "        \r";
			cout.flush();
		}

		auto const &entry = osmStore.pbf_relation_entry(i);

		currentTags.clear();
		for(auto const &i: entry.tags) {
			currentTags.emplace(std::piecewise_construct,
				std::forward_as_tuple(i.first.begin(), i.first.end()), 
				std::forward_as_tuple(i.second.begin(), i.second.end()));
		}

		output->setRelation(entry.relationId, entry.relationHandle, currentTags);
	}
}

/**
 *\brief The Main function is responsible for command line processing, loading data and starting worker threads.
 *
 * Data is loaded into OsmMemTiles and ShpMemTiles.
 *
 * Worker threads write the output tiles, and start in the outputProc function.
 */
int main(int argc, char* argv[]) {

	// ----	Read command-line options
	vector<string> inputFiles;
	string luaFile;
	string osmStoreFile;
	string osmStoreSettings;
	string jsonFile;
	uint threadNum;
	string outputFile;
	bool _verbose = false, sqlite= false, mergeSqlite = false, mapsplit = false, osmStoreCompact = false;
	bool index;

	po::options_description desc("tilemaker (c) 2016-2020 Richard Fairhurst and contributors\nConvert OpenStreetMap .pbf files into vector tiles\n\nAvailable options");
	desc.add_options()
		("help",                                                                 "show help message")
		("input",  po::value< vector<string> >(&inputFiles),                     "source .osm.pbf file")
		("output", po::value< string >(&outputFile),                             "target directory or .mbtiles/.sqlite file")
		("index",  po::bool_switch(&index),                                      "generate an index file from the specified input file")
		("merge"  ,po::bool_switch(&mergeSqlite),                                "merge with existing .mbtiles (overwrites otherwise)")
		("config", po::value< string >(&jsonFile)->default_value("config.json"), "config JSON file")
		("process",po::value< string >(&luaFile)->default_value("process.lua"),  "tag-processing Lua file")
		("store",  po::value< string >(&osmStoreFile),  "temporary storage for node/ways/relations data")
		("compact",  po::bool_switch(&osmStoreCompact),  "Use 32bits NodeIDs and reduce overall memory usage (compact mode).\nThis requires the input to be renumbered and the init-store to be configured")
		("init-store",  po::value< string >(&osmStoreSettings)->default_value("20:5"),  "initial number of millions of entries for the nodes (20M) and ways (5M)")
		("verbose",po::bool_switch(&_verbose),                                   "verbose error output")
		("threads",po::value< uint >(&threadNum)->default_value(0),              "number of threads (automatically detected if 0)");
	po::positional_options_description p;
	p.add("input", -1);
	po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
    } catch (const po::unknown_option& ex) {
        cerr << "Unknown option: " << ex.get_option_name() << endl;
        return -1;
    }
	po::notify(vm);
	
	if (vm.count("help")) { cout << desc << endl; return 0; }
	if (vm.count("output")==0) { cerr << "You must specify an output file or directory. Run with --help to find out more." << endl; return -1; }
	if (vm.count("input")==0) { cout << "No source .osm.pbf file supplied" << endl; }

	if (ends_with(outputFile, ".mbtiles") || ends_with(outputFile, ".sqlite")) { sqlite=true; }
	if (threadNum == 0) { threadNum = max(thread::hardware_concurrency(), 1u); }
	verbose = _verbose;


	// ---- Check config
	
	if (!boost::filesystem::exists(jsonFile)) { cerr << "Couldn't open .json config: " << jsonFile << endl; return -1; }
	if (!boost::filesystem::exists(luaFile )) { cerr << "Couldn't open .lua script: "  << luaFile  << endl; return -1; }

	// ---- Remove existing .mbtiles if it exists

	if (sqlite && !mergeSqlite && static_cast<bool>(std::ifstream(outputFile))) {
		cout << "mbtiles file exists, will overwrite (Ctrl-C to abort, rerun with --merge to keep)" << endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		if (remove(outputFile.c_str()) != 0) {
			cerr << "Couldn't remove existing file" << endl;
			return 0;
		}
	} else if (mergeSqlite && !static_cast<bool>(std::ifstream(outputFile))) {
		cout << "--merge specified but .mbtiles file doesn't already exist, ignoring" << endl;
		mergeSqlite = false;
	}

	// ----	Read bounding box from first .pbf (if there is one) or mapsplit file

	bool hasClippingBox = false;
	Box clippingBox;
	MBTiles mapsplitFile;
	double minLon=0.0, maxLon=0.0, minLat=0.0, maxLat=0.0;
	if (inputFiles.size()==1 && (ends_with(inputFiles[0], ".mbtiles") || ends_with(inputFiles[0], ".sqlite") || ends_with(inputFiles[0], ".msf"))) {
		mapsplit = true;
		mapsplitFile.openForReading(&inputFiles[0]);
		mapsplitFile.readBoundingBox(minLon, maxLon, minLat, maxLat);
		cout << "Bounding box " << minLon << ", " << maxLon << ", " << minLat << ", " << maxLat << endl;
		clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
		                  geom::make<Point>(maxLon, lat2latp(maxLat)));
		hasClippingBox = true;

	} else if (inputFiles.size()>0) {
		int ret = ReadPbfBoundingBox(inputFiles[0], minLon, maxLon, minLat, maxLat, hasClippingBox);
		if(ret != 0) return ret;
		if(hasClippingBox) {
			cout << "Bounding box " << minLon << ", " << maxLon << ", " << minLat << ", " << maxLat << endl;
			clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
			                  geom::make<Point>(maxLon, lat2latp(maxLat)));
		}
	}

	// ----	Read JSON config

	rapidjson::Document jsonConfig;
	class Config config;
	try {
		FILE* fp = fopen(jsonFile.c_str(), "r");
		char readBuffer[65536];
		rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
		jsonConfig.ParseStream(is);
		if (jsonConfig.HasParseError()) { cerr << "Invalid JSON file." << endl; return -1; }
		fclose(fp);

		config.readConfig(jsonConfig, hasClippingBox, clippingBox);
	} catch (...) {
		cerr << "Couldn't find expected details in JSON file." << endl;
		return -1;
	}

	uint storeNodesSize = 20;
	uint storeWaysSize = 5;

	try {
		vector<string> tokens;
		boost::split(tokens, osmStoreSettings, boost::is_any_of(":"));

		if(tokens.size() != 2) {
			cerr << "Invalid initial store configuration: " << osmStoreSettings << std::endl;
			return -1;
		}

		storeNodesSize = boost::lexical_cast<uint>(tokens[0]);
		storeWaysSize = boost::lexical_cast<uint>(tokens[1]);
		std::cout << "Initializing storage to " << storeNodesSize << "M nodes and " << storeWaysSize << "M ways" << std::endl;
	} catch(std::exception &e)
	{
		cerr << "Invalid parameter for store initial settings (" << osmStoreSettings << "): " << e.what() << endl;
		return -1;
	}

	// For each tile, objects to be used in processing
	std::unique_ptr<OSMStore> osmStore;
	if(osmStoreCompact) {
		std:: cout << "\nImportant: Tilemaker running in compact mode.\nUse 'osmium renumber' first if working with OpenStreetMap-sourced data,\ninitialize the init store to the highest NodeID that is stored in the input file.\n" << std::endl;
   		osmStore.reset(new OSMStoreImpl<NodeStoreCompact>());
	} else {
   		osmStore.reset(new OSMStoreImpl<NodeStore>());
	}

	std::string indexfilename = (inputFiles.empty() ? "tilemaker" : inputFiles[0]) + ".idx";
	if(index) { 
		std::cout << "Writing index to file: " << indexfilename << std::endl;
		osmStore->open(indexfilename, true);
	} else if(!osmStoreFile.empty()) {
		std::cout << "Using osm store file: " << osmStoreFile << std::endl;
		osmStore->open(osmStoreFile, true);
	}

   	osmStore->reserve(storeNodesSize * 1000000, storeWaysSize * 1000000);

	AttributeStore attributeStore;

	class OsmMemTiles osmMemTiles(config.baseZoom);
	class ShpMemTiles shpMemTiles(*osmStore, config.baseZoom);
	class LayerDefinition layers(config.layers);

	OsmLuaProcessing osmLuaProcessing(osmStore.get(), *osmStore, config, layers, luaFile, 
		shpMemTiles, osmMemTiles, attributeStore);

	// ---- Load external shp files

	for (size_t layerNum=0; layerNum<layers.layers.size(); layerNum++) {
		// External layer sources
		LayerDef &layer = layers.layers[layerNum];
		if(layer.indexed) { shpMemTiles.CreateNamedLayerIndex(layer.name); }

		if (layer.source.size()>0) {
			if (!hasClippingBox) {
				cerr << "Can't read shapefiles unless a bounding box is provided." << endl;
				exit(EXIT_FAILURE);
			}
			cout << "Reading .shp " << layer.name << endl;
			readShapefile(clippingBox,
			              layers,
			              config.baseZoom, layerNum,
						  shpMemTiles, osmLuaProcessing);
		}
	}

	// ----	Read significant node tags

	vector<string> nodeKeyVec = osmLuaProcessing.GetSignificantNodeKeys();
	unordered_set<string> nodeKeys(nodeKeyVec.begin(), nodeKeyVec.end());

	// ----	Read all PBFs
	
	PbfReader pbfReader(*osmStore);
	pbfReader.output = &osmLuaProcessing;

	std::unique_ptr<PbfIndexWriter> indexWriter;

	if(index) {
		std::cout << "Generating index file " << std::endl;
		indexWriter.reset(new PbfIndexWriter(*osmStore));
		pbfReader.output = indexWriter.get();
	}

	if (!mapsplit) {
		if(!index && boost::filesystem::exists(indexfilename)) {
			std::unique_ptr<OSMStore> indexStore;
			if(osmStoreCompact)
	   			indexStore.reset(new OSMStoreImpl<NodeStoreCompact>());
			else
   				indexStore.reset(new OSMStoreImpl<NodeStore>());
	   		indexStore->reserve(storeNodesSize * 1000000, storeWaysSize * 1000000);
	
			std::cout << "Using index to generate tiles: " << indexfilename << std::endl;
			indexStore->open(indexfilename, false);
			osmLuaProcessing.setIndexStore(indexStore.get());
			generate_from_index(*indexStore, &osmLuaProcessing);
		} else {
			
			for (auto inputFile : inputFiles) {
				cout << "Reading .pbf " << inputFile << endl;
				
				ifstream infile(inputFile, ios::in | ios::binary);
				if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }

				int ret = pbfReader.ReadPbfFile(infile, nodeKeys);
				if (ret != 0) return ret;
			} 
		}

	}

	if(index) {
		return 0;
	}

	// ----	Initialise SharedData
	std::vector<class TileDataSource *> sources = {&osmMemTiles, &shpMemTiles};

	class SharedData sharedData(config, layers);
	sharedData.outputFile = outputFile;
	sharedData.sqlite = sqlite;
	sharedData.mergeSqlite = mergeSqlite;

	// ----	Initialise mbtiles if required
	
	if (sharedData.sqlite) {
		sharedData.mbtiles.openForWriting(&sharedData.outputFile);
		sharedData.mbtiles.writeMetadata("name",sharedData.config.projectName);
		sharedData.mbtiles.writeMetadata("type","baselayer");
		sharedData.mbtiles.writeMetadata("version",sharedData.config.projectVersion);
		sharedData.mbtiles.writeMetadata("description",sharedData.config.projectDesc);
		sharedData.mbtiles.writeMetadata("format","pbf");
		sharedData.mbtiles.writeMetadata("minzoom",to_string(sharedData.config.startZoom));
		sharedData.mbtiles.writeMetadata("maxzoom",to_string(sharedData.config.endZoom));
		if (!sharedData.config.defaultView.empty()) { sharedData.mbtiles.writeMetadata("center",sharedData.config.defaultView); }

		ostringstream bounds;
		if (mergeSqlite) {
			double cMinLon, cMaxLon, cMinLat, cMaxLat;
			sharedData.mbtiles.readBoundingBox(cMinLon, cMaxLon, cMinLat, cMaxLat);
			sharedData.config.enlargeBbox(cMinLon, cMaxLon, cMinLat, cMaxLat);
		}
		bounds << fixed << sharedData.config.minLon << "," << sharedData.config.minLat << "," << sharedData.config.maxLon << "," << sharedData.config.maxLat;
		sharedData.mbtiles.writeMetadata("bounds",bounds.str());
	}

	// ----	Write out data

	// If mapsplit, read list of tiles available
	unsigned runs=1;
	vector<tuple<int,int,int>> tileList;
	if (mapsplit) {
		mapsplitFile.readTileList(tileList);
		runs = tileList.size();
	}

	for (unsigned run=0; run<runs; run++) {
		// Read mapsplit tile and parse, if applicable
		int srcZ = -1, srcX = -1, srcY = -1, tmsY = -1;

		if (mapsplit) {
			osmMemTiles.Clear();

			tie(srcZ,srcX,tmsY) = tileList.back();
			srcY = pow(2,srcZ) - tmsY - 1; // TMS
			if (srcZ > config.baseZoom) {
				cerr << "Mapsplit tiles (zoom " << srcZ << ") must not be greater than basezoom " << config.baseZoom << endl;
				return 0;
			} else if (srcZ > config.startZoom) {
				cout << "Mapsplit tiles (zoom " << srcZ << ") can't write data at zoom level " << config.startZoom << endl;
			}

			cout << "Reading tile " << srcZ << ": " << srcX << "," << srcY << " (" << (run+1) << "/" << runs << ")" << endl;
			vector<char> pbf = mapsplitFile.readTile(srcZ,srcX,tmsY);

			boost::interprocess::bufferstream pbfstream(pbf.data(), pbf.size(),  ios::in | ios::binary);
			pbfReader.ReadPbfFile(pbfstream, nodeKeys);

			tileList.pop_back();
		}

		// Launch the pool with threadNum threads
		boost::asio::thread_pool pool(threadNum);

		// Mutex is hold when IO is performed
		std::mutex io_mutex;

		// Loop through tiles
		std::size_t tc = 0;

		std::deque< std::pair<unsigned int, TileCoordinates> > tile_coordinates;
		for (uint zoom=sharedData.config.startZoom; zoom<=sharedData.config.endZoom; zoom++) {
			auto zoom_result = GetTileCoordinates(sources, zoom);
			for(auto&& it: zoom_result) {
				// If we're constrained to a source tile, check we're within it
				if (srcZ>-1) {
					int x = it.x / pow(2, zoom-srcZ);
					int y = it.y / pow(2, zoom-srcZ);
					if (x!=srcX || y!=srcY) continue;
				}
			
				if (hasClippingBox) {
					if(!boost::geometry::intersects(TileBbox(it, zoom).getTileBox(), clippingBox)) 
						continue;
				}

				tile_coordinates.push_back(std::make_pair(zoom, it));
			}
		}

		std::size_t interval = 100;
		for(std::size_t start_index = 0; start_index < tile_coordinates.size(); start_index += interval) {

			boost::asio::post(pool, [=, &tile_coordinates, &pool, &sharedData, &osmStore, &io_mutex, &tc]() {
				std::size_t end_index = std::min(tile_coordinates.size(), start_index + interval);
				for(std::size_t i = start_index; i < end_index; ++i) {
					unsigned int zoom = tile_coordinates[i].first;
					TileCoordinates coords = tile_coordinates[i].second;
					outputProc(pool, sharedData, *osmStore, GetTileData(sources, coords, zoom), coords, zoom);
				}

				const std::lock_guard<std::mutex> lock(io_mutex);
				tc += (end_index - start_index); 

				unsigned int zoom = tile_coordinates[end_index - 1].first;
				cout << "Zoom level " << zoom << ", writing tile " << tc << " of " << tile_coordinates.size() << "               \r" << std::flush;
			});
		}
		
		// Wait for all tasks in the pool to complete.
		pool.join();
	}

	// ----	Close tileset

	if (sqlite)
		WriteSqliteMetadata(jsonConfig, sharedData, layers);
	else 
		WriteFileMetadata(jsonConfig, sharedData, layers);

	google::protobuf::ShutdownProtobufLibrary();

#ifndef _MSC_VER
	if (verbose) {
		struct rusage r_usage;
		getrusage(RUSAGE_SELF, &r_usage);
		cout << "\nMemory used: " << r_usage.ru_maxrss << endl;
	}
#endif

	cout << endl << "Filled the tileset with good things at " << sharedData.outputFile << endl;
}

