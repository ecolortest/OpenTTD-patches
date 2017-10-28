/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file animated_tile_sl.cpp Code handling saving and loading of animated tiles */

#include "../stdafx.h"
#include "../core/alloc_func.hpp"
#include "../map/coord.h"

#include "saveload_buffer.h"

extern TileIndex *_animated_tile_list;
extern uint _animated_tile_count;
extern uint _animated_tile_allocated;

/**
 * Save the ANIT chunk.
 */
static void Save_ANIT(SaveDumper *dumper)
{
	dumper->WriteRIFFSize(_animated_tile_count * sizeof(*_animated_tile_list));
	for (uint i = 0; i < _animated_tile_count; i++) {
		dumper->WriteUint32 (_animated_tile_list[i]);
	}
}

/**
 * Load the ANIT chunk; the chunk containing the animated tiles.
 */
static void Load_ANIT(LoadBuffer *reader)
{
	/* Before legacy version 80 we did NOT have a variable length animated tile table */
	if (reader->IsOTTDVersionBefore(80)) {
		/* In pre version 6, we has 16bit per tile, now we have 32bit per tile, convert it ;) */
		bool pre6 = reader->IsOTTDVersionBefore (6);
		for (_animated_tile_count = 0; _animated_tile_count < 256; _animated_tile_count++) {
			uint32 tile = pre6 ? reader->ReadUint16() : reader->ReadUint32();
			if (tile == 0) {
				reader->Skip ((255 - _animated_tile_count) * (pre6 ? 2 : 4));
				break;
			}
			_animated_tile_list[_animated_tile_count] = tile;
		}
		return;
	}

	_animated_tile_count = (uint)reader->GetChunkSize() / sizeof(*_animated_tile_list);

	/* Determine a nice rounded size for the amount of allocated tiles */
	_animated_tile_allocated = 256;
	while (_animated_tile_allocated < _animated_tile_count) _animated_tile_allocated *= 2;

	_animated_tile_list = xrealloct<TileIndex>(_animated_tile_list, _animated_tile_allocated);
	for (uint i = 0; i < _animated_tile_count; i++) {
		_animated_tile_list[i] = reader->ReadUint32();
	}
}

/**
 * "Definition" imported by the saveload code to be able to load and save
 * the animated tile table.
 */
extern const ChunkHandler _animated_tile_chunk_handlers[] = {
	{ 'ANIT', Save_ANIT, Load_ANIT, NULL, NULL, CH_RIFF | CH_LAST},
};
