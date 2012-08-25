/*
 * =====================================================================
 *        Version:  1.0
 *        Created:  23.08.2012 12:35:53
 *         Author:  Miroslav Bendík
 *        Company:  LinuxOS.sk
 * =====================================================================
 */

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <gdfontmb.h>
#include <iostream>
#include <sstream>
#include <zlib.h>
#include <boost/filesystem.hpp>
#include "TileGenerator.h"

using namespace std;

static inline sqlite3_int64 pythonmodulo(sqlite3_int64 i, sqlite3_int64 mod)
{
	if (i >= 0) {
		return i % mod;
	}
	else {
		return mod - ((-i) % mod);
	}
}

static inline int unsignedToSigned(long i, long max_positive)
{
	if (i < max_positive) {
		return i;
	}
	else {
		return i - 2l * max_positive;
	}
}

static inline int readU16(const char *data)
{
	return int(data[0]) * 256 + data[1];
}

static inline int rgb2int(uint8_t r, uint8_t g, uint8_t b)
{
	return (r << 16) + (g << 8) + b;
}

static inline int readBlockContent(const unsigned char *mapData, int version, int datapos)
{
	if (version >= 24) {
		size_t index = datapos << 1;
		return (mapData[index] << 8) | mapData[index + 1];
	}
	else if (version >= 20) {
		if (mapData[datapos] <= 0x80) {
			return mapData[datapos];
		}
		else {
			return (int(mapData[datapos]) << 4) | (int(mapData[datapos + 0x2000]) >> 4);
		}
	}
	else {
		throw VersionError();
	}
}

static inline std::string zlibDecompress(const char *data, std::size_t size, std::size_t *processed)
{
	string buffer;
	const size_t BUFSIZE = 128 * 1024;
	uint8_t temp_buffer[BUFSIZE];

	z_stream strm;
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.next_in = Z_NULL;
	strm.avail_in = size;

	if (inflateInit(&strm) != Z_OK) {
		throw DecompressError();
	}

	strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
	int ret = 0;
	do {
		strm.avail_out = BUFSIZE;
		strm.next_out = temp_buffer;
		ret = inflate(&strm, Z_NO_FLUSH);
		buffer += string(reinterpret_cast<char *>(temp_buffer), BUFSIZE - strm.avail_out);
	} while (ret == Z_OK);
	if (ret != Z_STREAM_END) {
		throw DecompressError();
	}
	*processed = (const char *)strm.next_in - (const char *)data;
	(void)inflateEnd(&strm);

	return buffer;
}

static inline int colorSafeBounds(int color)
{
	if (color > 255) {
		return 255;
	}
	else if (color < 0) {
		return 0;
	}
	else {
		return color;
	}
}

TileGenerator::TileGenerator():
	m_bgColor(255, 255, 255),
	m_scaleColor(0, 0, 0),
	m_originColor(255, 0, 0),
	m_playerColor(255, 0, 0),
	m_drawOrigin(false),
	m_drawPlayers(false),
	m_drawScale(false),
	m_border(0),
	m_db(0),
	m_image(0),
	m_xMin(0),
	m_xMax(0),
	m_zMin(0),
	m_zMax(0)
{
}

TileGenerator::~TileGenerator()
{
	if (m_db != 0) {
		sqlite3_close(m_db);
		m_db = 0;
	}
}

void TileGenerator::setBgColor(const std::string &bgColor)
{
	m_bgColor = parseColor(bgColor);
}

void TileGenerator::setScaleColor(const std::string &scaleColor)
{
	m_scaleColor = parseColor(scaleColor);
}

void TileGenerator::setOriginColor(const std::string &originColor)
{
	m_originColor = parseColor(originColor);
}

void TileGenerator::setPlayerColor(const std::string &playerColor)
{
	m_playerColor = parseColor(playerColor);
}

Color TileGenerator::parseColor(const std::string &color)
{
	Color parsed;
	if (color.length() != 7) {
		throw ColorError();
	}
	if (color[0] != '#') {
		throw ColorError();
	}
	long col = strtol(color.c_str() + 1, NULL, 16);
	parsed.b = col % 256;
	col = col / 256;
	parsed.g = col % 256;
	col = col / 256;
	parsed.r = col % 256;
	return parsed;
}

void TileGenerator::setDrawOrigin(bool drawOrigin)
{
	m_drawOrigin = drawOrigin;
}

void TileGenerator::setDrawPlayers(bool drawPlayers)
{
	m_drawPlayers = drawPlayers;
}

void TileGenerator::setDrawScale(bool drawScale)
{
	m_drawScale = drawScale;
	if (m_drawScale) {
		m_border = 40;
	}
}

void TileGenerator::parseColorsFile(const std::string &fileName)
{
	ifstream in;
	in.open(fileName.c_str(), ifstream::in);
	if (!in.is_open()) {
		std::cerr << "File colors.txt does not exist" << std::endl;
		exit(-2);
	}

	while (in.good()) {
		string name;
		Color color;
		in >> name;
		if (name[0] == '#') {
			in.ignore(65536, '\n');
			in >> name;
		}
		while (name == "\n" && in.good()) {
			in >> name;
		}
		int r, g, b;
		in >> r;
		in >> g;
		in >> b;
		if (in.good()) {
			color.r = r;
			color.g = g;
			color.b = b;
			m_colors[name] = color;
		}
	}
}

void TileGenerator::generate(const std::string &input, const std::string &output)
{
	openDb(input);
	loadBlocks();
	createImage();
	renderMap();
	if (m_drawScale) {
		renderScale();
	}
	if (m_drawOrigin) {
		renderOrigin();
	}
	if (m_drawPlayers) {
		renderPlayers(input);
	}
	writeImage(output);
}

void TileGenerator::openDb(const std::string &input)
{
	string db_name = input + "map.sqlite";
	if (sqlite3_open(db_name.c_str(), &m_db) != SQLITE_OK) {
		throw DbError();
	}
}

void TileGenerator::loadBlocks()
{
	sqlite3_stmt *statement;
	string sql = "SELECT pos FROM blocks";
	if (sqlite3_prepare_v2(m_db, sql.c_str(), sql.length(), &statement, 0) == SQLITE_OK) {
		int result = 0;
		while (true) {
			result = sqlite3_step(statement);
			if(result == SQLITE_ROW) {
				sqlite3_int64 blocknum = sqlite3_column_int64(statement, 0);
				BlockPos pos = decodeBlockPos(blocknum);
				if (pos.x > SectorXMax || pos.x < SectorXMin || pos.z > SectorZMax || pos.z < SectorZMin) {
					continue;
				}
				if (pos.x < m_xMin) {
					m_xMin = pos.x;
				}
				if (pos.x > m_xMax) {
					m_xMax = pos.x;
				}
				if (pos.z < m_zMin) {
					m_zMin = pos.z;
				}
				if (pos.z > m_zMax) {
					m_zMax = pos.z;
				}
				m_positions.push_back(std::pair<int, int>(pos.x, pos.z));
			}
			else {
				break;
			}
		}
		m_positions.sort();
		m_positions.unique();
	}
	else {
		throw DbError();
	}
}

inline BlockPos TileGenerator::decodeBlockPos(sqlite3_int64 blockId) const
{
	BlockPos pos;
	pos.x = unsignedToSigned(pythonmodulo(blockId, 4096), 2048);
	blockId = (blockId - pos.x) / 4096;
	pos.y = unsignedToSigned(pythonmodulo(blockId, 4096), 2048);
	blockId = (blockId - pos.y) / 4096;
	pos.z = unsignedToSigned(pythonmodulo(blockId, 4096), 2048);
	return pos;
}

void TileGenerator::createImage()
{
	m_mapWidth = (m_xMax - m_xMin + 1) * 16;
	m_mapHeight = (m_zMax - m_zMin + 1) * 16;
	m_image = gdImageCreateTrueColor(m_mapWidth + m_border, m_mapHeight + m_border);
	m_blockPixelAttributes.setWidth(m_mapWidth);
	// Background
	gdImageFilledRectangle(m_image, 0, 0, m_mapWidth + m_border - 1, m_mapHeight + m_border -1, rgb2int(m_bgColor.r, m_bgColor.g, m_bgColor.b));
}

void TileGenerator::renderMap()
{
	sqlite3_stmt *statement;
	string sql = "SELECT pos, data FROM blocks WHERE (pos >= ? AND pos <= ?)";
	if (sqlite3_prepare_v2(m_db, sql.c_str(), sql.length(), &statement, 0) != SQLITE_OK) {
		throw DbError();
	}

	std::list<int> zlist = getZValueList();
	for (std::list<int>::iterator zPosition = zlist.begin(); zPosition != zlist.end(); ++zPosition) {
		int zPos = *zPosition;
		map<int, BlockList> blocks = getBlocksOnZ(zPos, statement);
		for (std::list<std::pair<int, int> >::const_iterator position = m_positions.begin(); position != m_positions.end(); ++position) {
			if (position->second != zPos) {
				continue;
			}

			for (int i = 0; i < 16; ++i) {
				m_readedPixels[i] = 0;
			}

			int xPos = position->first;
			blocks[xPos].sort();
			const BlockList &blockStack = blocks[xPos];
			for (BlockList::const_iterator it = blockStack.begin(); it != blockStack.end(); ++it) {
				const BlockPos &pos = it->first;
				const char *data = it->second.c_str();
				size_t length = it->second.length();

				uint8_t version = data[0];
				//uint8_t flags = data[1];

				size_t dataOffset = 0;
				if (version >= 22) {
					dataOffset = 4;
				}
				else {
					dataOffset = 2;
				}
				size_t processed;
				string mapData = zlibDecompress(data + dataOffset, length - dataOffset, &processed);
				dataOffset += processed;
				string mapMetadata = zlibDecompress(data + dataOffset, length - dataOffset, &processed);
				dataOffset += processed;

				// Skip unused data
				if (version <= 21) {
					dataOffset += 2;
				}
				if (version == 23) {
					dataOffset += 1;
				}
				if (version == 24) {
					uint8_t ver = data[dataOffset++];
					if (ver == 1) {
						int num = readU16(data + dataOffset);
						dataOffset += 2;
						dataOffset += 10 * num;
					}
				}

				// Skip unused static objects
				dataOffset++; // Skip static object version
				int staticObjectCount = readU16(data + dataOffset);
				dataOffset += 2;
				for (int i = 0; i < staticObjectCount; ++i) {
					dataOffset += 13;
					int dataSize = readU16(data + dataOffset);
					dataOffset += dataSize + 2;
				}
				dataOffset += 4; // Skip timestamp

				m_blockAirId = -1;
				m_blockIgnoreId = -1;
				// Read mapping
				if (version >= 22) {
					dataOffset++; // mapping version
					int numMappings = readU16(data + dataOffset);
					dataOffset += 2;
					for (int i = 0; i < numMappings; ++i) {
						int nodeId = readU16(data + dataOffset);
						dataOffset += 2;
						int nameLen = readU16(data + dataOffset);
						dataOffset += 2;
						string name = string(data + dataOffset, nameLen);
						if (name == "air") {
							m_blockAirId = nodeId;
						}
						else if (name == "ignore") {
							m_blockIgnoreId = nodeId;
						}
						else {
							m_nameMap[nodeId] = name;
						}
						dataOffset += nameLen;
					}
				}

				// Node timers
				if (version >= 25) {
					dataOffset++;
					int numTimers = readU16(data + dataOffset);
					dataOffset += 2;
					dataOffset += numTimers * 10;
				}

				renderMapBlock(mapData, pos, version);

				bool allReaded = true;
				for (int i = 0; i < 16; ++i) {
					if (m_readedPixels[i] != 0xffff) {
						allReaded = false;
					}
				}
				if (allReaded) {
					break;
				}
			}
		}
		renderShading(zPos);
	}
}

inline void TileGenerator::renderMapBlock(const std::string &mapBlock, const BlockPos &pos, int version)
{
	int xBegin = (pos.x - m_xMin) * 16;
	int zBegin = (m_zMax - pos.z) * 16;
	const unsigned char *mapData = reinterpret_cast<const unsigned char *>(mapBlock.c_str());
	for (int z = 0; z < 16; ++z) {
		int imageY = getImageY(zBegin + 15 - z);
		for (int x = 0; x < 16; ++x) {
			if (m_readedPixels[z] & (1 << x)) {
				continue;
			}
			int imageX = getImageX(xBegin + x);
			for (int y = 15; y >= 0; --y) {
				int position = x + (y << 4) + (z << 8);
				int content = readBlockContent(mapData, version, position);
				if (content == m_blockIgnoreId || content == m_blockAirId) {
					continue;
				}
				std::map<int, std::string>::iterator blockName = m_nameMap.find(content);
				if (blockName != m_nameMap.end()) {
					const string &name = blockName->second;
					ColorMap::const_iterator color = m_colors.find(name);
					if (color != m_colors.end()) {
						const Color &c = color->second;
						m_image->tpixels[imageY][imageX] = rgb2int(c.r, c.g, c.b);
						m_readedPixels[z] |= (1 << x);
						m_blockPixelAttributes.attribute(15 - z, xBegin + x).height = pos.y * 16 + y;
					}
					break;
				}
			}
		}
	}
}

inline void TileGenerator::renderShading(int zPos)
{
	int zBegin = (m_zMax - zPos) * 16;
	for (int z = 1; z < 17; ++z) {
		int imageY = zBegin + z;
		if (imageY >= m_mapHeight) {
			continue;
		}
		imageY = getImageY(imageY);
		for (int x = 0; x < m_mapWidth; ++x) {
			if (!m_blockPixelAttributes.attribute(z, x).valid_height() || !m_blockPixelAttributes.attribute(z, x - 1).valid_height() || !m_blockPixelAttributes.attribute(z - 1, x).valid_height()) {
				continue;
			}
			int y = m_blockPixelAttributes.attribute(z, x).height;
			int y1 = m_blockPixelAttributes.attribute(z, x - 1).height;
			int y2 = m_blockPixelAttributes.attribute(z - 1, x).height;
			int d = ((y - y1) + (y - y2)) * 12;
			if (d > 36) {
				d = 36;
			}
			int sourceColor = m_image->tpixels[imageY][getImageX(x)] & 0xffffff;
			int r = (sourceColor & 0xff0000) >> 16;
			int g = (sourceColor & 0x00ff00) >> 8;
			int b = (sourceColor & 0x0000ff);
			r = colorSafeBounds(r + d);
			g = colorSafeBounds(g + d);
			b = colorSafeBounds(b + d);
			m_image->tpixels[imageY][getImageX(x)] = rgb2int(r, g, b);
		}
	}
	m_blockPixelAttributes.scroll();
}

void TileGenerator::renderScale()
{
	int color = rgb2int(m_scaleColor.r, m_scaleColor.g, m_scaleColor.b);
	gdImageString(m_image, gdFontGetMediumBold(), 24, 0, reinterpret_cast<unsigned char *>(const_cast<char *>("X")), color);
	gdImageString(m_image, gdFontGetMediumBold(), 2, 24, reinterpret_cast<unsigned char *>(const_cast<char *>("Z")), color);

	string scaleText;

	for (int i = (m_xMin / 4) * 4; i <= m_xMax; i += 4) {
		stringstream buf;
		buf << i * 16;
		scaleText = buf.str();

		int xPos = m_xMin * -16 + i * 16 + m_border;
		gdImageString(m_image, gdFontGetMediumBold(), xPos + 2, 0, reinterpret_cast<unsigned char *>(const_cast<char *>(scaleText.c_str())), color);
		gdImageLine(m_image, xPos, 0, xPos, m_border - 1, color);
	}

	for (int i = (m_zMax / 4) * 4; i >= m_zMin; i -= 4) {
		stringstream buf;
		buf << i * 16;
		scaleText = buf.str();

		int yPos = m_mapHeight - 1 - (i * 16 - m_zMin * 16) + m_border;
		gdImageString(m_image, gdFontGetMediumBold(), 2, yPos, reinterpret_cast<unsigned char *>(const_cast<char *>(scaleText.c_str())), color);
		gdImageLine(m_image, 0, yPos, m_border - 1, yPos, color);
	}
}

void TileGenerator::renderOrigin()
{
	int imageX = -m_xMin * 16 + m_border;
	int imageY = m_mapHeight - m_zMin * -16 + m_border;
	gdImageArc(m_image, imageX, imageY, 12, 12, 0, 360, rgb2int(m_originColor.r, m_originColor.g, m_originColor.b));
}

void TileGenerator::renderPlayers(const std::string &inputPath)
{
	int color = rgb2int(m_playerColor.r, m_playerColor.g, m_playerColor.b);

	string playersPath = inputPath + "players";
	boost::filesystem::path path(playersPath.c_str());
	boost::filesystem::directory_iterator end_iter;
	boost::filesystem::directory_iterator iter(path);
	for (; iter != end_iter; ++iter) {
		if (!boost::filesystem::is_directory(iter->status())) {
			string path = iter->path().string();

			ifstream in;
			in.open(path.c_str(), ifstream::in);
			string buffer;
			string name;
			string position;
			while (getline(in, buffer)) {
				if (buffer.find("name = ") == 0) {
					name = buffer.substr(7);
				}
				else if (buffer.find("position = ") == 0) {
					position = buffer.substr(12, buffer.length() - 13);
				}
			}
			double x, y, z;
			char comma;
			istringstream positionStream(position, istringstream::in);
			positionStream >> x;
			positionStream >> comma;
			positionStream >> y;
			positionStream >> comma;
			positionStream >> z;
			int imageX = x / 10 - m_xMin * 16 + m_border;
			int imageY = m_mapHeight - (z / 10 - m_zMin * 16) + m_border;

			gdImageArc(m_image, imageX, imageY, 5, 5, 0, 360, color);
			gdImageString(m_image, gdFontGetMediumBold(), imageX + 2, imageY + 2, reinterpret_cast<unsigned char *>(const_cast<char *>(name.c_str())), color);
		}
	}
}

inline std::list<int> TileGenerator::getZValueList() const
{
	std::list<int> zlist;
	for (std::list<std::pair<int, int> >::const_iterator position = m_positions.begin(); position != m_positions.end(); ++position) {
		zlist.push_back(position->second);
	}
	zlist.sort();
	zlist.unique();
	return zlist;
}

std::map<int, TileGenerator::BlockList> TileGenerator::getBlocksOnZ(int zPos, sqlite3_stmt *statement) const
{
	map<int, BlockList> blocks;

	sqlite3_int64 psMin;
	sqlite3_int64 psMax;

	psMin = (static_cast<sqlite3_int64>(zPos) * 16777216l) - 0x800000;
	psMax = (static_cast<sqlite3_int64>(zPos) * 16777216l) + 0x7fffff;
	sqlite3_bind_int64(statement, 1, psMin);
	sqlite3_bind_int64(statement, 2, psMax);

	int result = 0;
	while (true) {
		result = sqlite3_step(statement);
		if(result == SQLITE_ROW) {
			sqlite3_int64 blocknum = sqlite3_column_int64(statement, 0);
			const char *data = reinterpret_cast<const char *>(sqlite3_column_blob(statement, 1));
			int size = sqlite3_column_bytes(statement, 1);
			BlockPos pos = decodeBlockPos(blocknum);
			blocks[pos.x].push_back(Block(pos, string(data, size)));
		}
		else {
			break;
		}
	}
	sqlite3_reset(statement);

	return blocks;
}

void TileGenerator::writeImage(const std::string &output)
{
	FILE *out;
	out = fopen(output.c_str(), "w");
	gdImagePng(m_image, out);
	fclose(out);
	gdImageDestroy(m_image);
}

inline int TileGenerator::getImageX(int val) const
{
	return val + m_border;
}

inline int TileGenerator::getImageY(int val) const
{
	return val + m_border;
}

