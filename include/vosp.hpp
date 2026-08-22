#pragma once

/**
 * @file vosp.hpp
 * @brief Public entry point for error, logging, worker-pool, and version APIs.
 */

#include <vosp/error.hpp>
#include <vosp/logger.hpp>
#include <vosp/version.hpp>
#include <vosp/worker_pool.hpp>

namespace vosp {
/** @brief Short facade name; equivalent to vosp::logger::Logger. */
using logger::Logger;
} // namespace vosp

/** @brief Compact namespace facade for the complete VOSP ecosystem. */
namespace vsp = vosp;
