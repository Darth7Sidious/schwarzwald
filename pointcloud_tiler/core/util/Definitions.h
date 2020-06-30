#pragma once

#include <experimental/filesystem>
#include <string>

namespace fs = std::experimental::filesystem;

/**
 * Different output formats
 */
enum class OutputFormat
{
  BIN,
  CZM_3DTILES,
  LAS,
  LAZ,
  ENTWINE_LAS,
  ENTWINE_LAZ
};

/**
 * Named constants for using compression
 */
enum class Compressed
{
  No,
  Yes
};

namespace progress {
const static std::string LOADING{ "loading" };
const static std::string INDEXING{ "indexing" };
const static std::string CONVERTING{ "converting" };
const static std::string GENERATING_TILESETS{ "generating tilesets" };
} // namespace progress