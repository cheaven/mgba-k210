/* Copyright (c) 2013-2017 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/ds/gx.h>

#include <mgba/internal/ds/ds.h>
#include <mgba/internal/ds/io.h>

mLOG_DEFINE_CATEGORY(DS_GX, "DS GX");

#define DS_GX_FIFO_SIZE 256
#define DS_GX_PIPE_SIZE 4

static void DSGXDummyRendererInit(struct DSGXRenderer* renderer);
static void DSGXDummyRendererReset(struct DSGXRenderer* renderer);
static void DSGXDummyRendererDeinit(struct DSGXRenderer* renderer);
static void DSGXDummyRendererSetRAM(struct DSGXRenderer* renderer, struct DSGXVertex* verts, struct DSGXPolygon* polys, unsigned polyCount);
static void DSGXDummyRendererDrawScanline(struct DSGXRenderer* renderer, int y);
static void DSGXDummyRendererGetScanline(struct DSGXRenderer* renderer, int y, color_t** output);

static const int32_t _gxCommandCycleBase[DS_GX_CMD_MAX] = {
	[DS_GX_CMD_NOP] = 0,
	[DS_GX_CMD_MTX_MODE] = 2,
	[DS_GX_CMD_MTX_PUSH] = 34,
	[DS_GX_CMD_MTX_POP] = 72,
	[DS_GX_CMD_MTX_STORE] = 34,
	[DS_GX_CMD_MTX_RESTORE] = 72,
	[DS_GX_CMD_MTX_IDENTITY] = 38,
	[DS_GX_CMD_MTX_LOAD_4x4] = 68,
	[DS_GX_CMD_MTX_LOAD_4x3] = 60,
	[DS_GX_CMD_MTX_MULT_4x4] = 70,
	[DS_GX_CMD_MTX_MULT_4x3] = 62,
	[DS_GX_CMD_MTX_MULT_3x3] = 56,
	[DS_GX_CMD_MTX_SCALE] = 44,
	[DS_GX_CMD_MTX_TRANS] = 44,
	[DS_GX_CMD_COLOR] = 2,
	[DS_GX_CMD_NORMAL] = 18,
	[DS_GX_CMD_TEXCOORD] = 2,
	[DS_GX_CMD_VTX_16] = 18,
	[DS_GX_CMD_VTX_10] = 16,
	[DS_GX_CMD_VTX_XY] = 16,
	[DS_GX_CMD_VTX_XZ] = 16,
	[DS_GX_CMD_VTX_YZ] = 16,
	[DS_GX_CMD_VTX_DIFF] = 16,
	[DS_GX_CMD_POLYGON_ATTR] = 2,
	[DS_GX_CMD_TEXIMAGE_PARAM] = 2,
	[DS_GX_CMD_PLTT_BASE] = 2,
	[DS_GX_CMD_DIF_AMB] = 8,
	[DS_GX_CMD_SPE_EMI] = 8,
	[DS_GX_CMD_LIGHT_VECTOR] = 12,
	[DS_GX_CMD_LIGHT_COLOR] = 2,
	[DS_GX_CMD_SHININESS] = 64,
	[DS_GX_CMD_BEGIN_VTXS] = 2,
	[DS_GX_CMD_END_VTXS] = 2,
	[DS_GX_CMD_SWAP_BUFFERS] = 784,
	[DS_GX_CMD_VIEWPORT] = 2,
	[DS_GX_CMD_BOX_TEST] = 206,
	[DS_GX_CMD_POS_TEST] = 18,
	[DS_GX_CMD_VEC_TEST] = 10,
};

static const int32_t _gxCommandParams[DS_GX_CMD_MAX] = {
	[DS_GX_CMD_MTX_MODE] = 1,
	[DS_GX_CMD_MTX_POP] = 1,
	[DS_GX_CMD_MTX_STORE] = 1,
	[DS_GX_CMD_MTX_RESTORE] = 1,
	[DS_GX_CMD_MTX_LOAD_4x4] = 16,
	[DS_GX_CMD_MTX_LOAD_4x3] = 12,
	[DS_GX_CMD_MTX_MULT_4x4] = 16,
	[DS_GX_CMD_MTX_MULT_4x3] = 12,
	[DS_GX_CMD_MTX_MULT_3x3] = 9,
	[DS_GX_CMD_MTX_SCALE] = 3,
	[DS_GX_CMD_MTX_TRANS] = 3,
	[DS_GX_CMD_COLOR] = 1,
	[DS_GX_CMD_NORMAL] = 1,
	[DS_GX_CMD_TEXCOORD] = 1,
	[DS_GX_CMD_VTX_16] = 2,
	[DS_GX_CMD_VTX_10] = 1,
	[DS_GX_CMD_VTX_XY] = 1,
	[DS_GX_CMD_VTX_XZ] = 1,
	[DS_GX_CMD_VTX_YZ] = 1,
	[DS_GX_CMD_VTX_DIFF] = 1,
	[DS_GX_CMD_POLYGON_ATTR] = 1,
	[DS_GX_CMD_TEXIMAGE_PARAM] = 1,
	[DS_GX_CMD_PLTT_BASE] = 1,
	[DS_GX_CMD_DIF_AMB] = 1,
	[DS_GX_CMD_SPE_EMI] = 1,
	[DS_GX_CMD_LIGHT_VECTOR] = 1,
	[DS_GX_CMD_LIGHT_COLOR] = 1,
	[DS_GX_CMD_SHININESS] = 32,
	[DS_GX_CMD_BEGIN_VTXS] = 1,
	[DS_GX_CMD_SWAP_BUFFERS] = 1,
	[DS_GX_CMD_VIEWPORT] = 1,
	[DS_GX_CMD_BOX_TEST] = 3,
	[DS_GX_CMD_POS_TEST] = 2,
	[DS_GX_CMD_VEC_TEST] = 1,
};

static struct DSGXRenderer dummyRenderer = {
	.init = DSGXDummyRendererInit,
	.reset = DSGXDummyRendererReset,
	.deinit = DSGXDummyRendererDeinit,
	.setRAM = DSGXDummyRendererSetRAM,
	.drawScanline = DSGXDummyRendererDrawScanline,
	.getScanline = DSGXDummyRendererGetScanline,
};

static void _pullPipe(struct DSGX* gx) {
	if (CircleBufferSize(&gx->fifo) >= sizeof(struct DSGXEntry)) {
		struct DSGXEntry entry = { 0 };
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.command);
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.params[0]);
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.params[1]);
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.params[2]);
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.params[3]);
		CircleBufferWrite8(&gx->pipe, entry.command);
		CircleBufferWrite8(&gx->pipe, entry.params[0]);
		CircleBufferWrite8(&gx->pipe, entry.params[1]);
		CircleBufferWrite8(&gx->pipe, entry.params[2]);
		CircleBufferWrite8(&gx->pipe, entry.params[3]);
	}
	if (CircleBufferSize(&gx->fifo) >= sizeof(struct DSGXEntry)) {
		struct DSGXEntry entry = { 0 };
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.command);
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.params[0]);
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.params[1]);
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.params[2]);
		CircleBufferRead8(&gx->fifo, (int8_t*) &entry.params[3]);
		CircleBufferWrite8(&gx->pipe, entry.command);
		CircleBufferWrite8(&gx->pipe, entry.params[0]);
		CircleBufferWrite8(&gx->pipe, entry.params[1]);
		CircleBufferWrite8(&gx->pipe, entry.params[2]);
		CircleBufferWrite8(&gx->pipe, entry.params[3]);
	}
}

static void _fifoRun(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct DSGX* gx = context;
	uint32_t cycles;
	bool first = true;
	while (!gx->swapBuffers) {
		if (CircleBufferSize(&gx->pipe) <= 2 * sizeof(struct DSGXEntry)) {
			_pullPipe(gx);
		}

		if (!CircleBufferSize(&gx->pipe)) {
			cycles = 0;
			break;
		}

		DSRegGXSTAT gxstat = gx->p->memory.io9[DS9_REG_GXSTAT_LO >> 1];
		int pvMatrixPointer = DSRegGXSTATGetPVMatrixStackLevel(gxstat);
		int projMatrixPointer = DSRegGXSTATGetProjMatrixStackLevel(gxstat);

		struct DSGXEntry entry = { 0 };
		CircleBufferDump(&gx->pipe, (int8_t*) &entry.command, 1);
		cycles = _gxCommandCycleBase[entry.command];

		if (first) {
			first = false;
		} else if (cycles > cyclesLate) {
			break;
		}
		CircleBufferRead8(&gx->pipe, (int8_t*) &entry.command);
		CircleBufferRead8(&gx->pipe, (int8_t*) &entry.params[0]);
		CircleBufferRead8(&gx->pipe, (int8_t*) &entry.params[1]);
		CircleBufferRead8(&gx->pipe, (int8_t*) &entry.params[2]);
		CircleBufferRead8(&gx->pipe, (int8_t*) &entry.params[3]);

		if (gx->activeParams) {
			int index = _gxCommandParams[entry.command] - gx->activeParams;
			gx->activeEntries[index] = entry;
			--gx->activeParams;
		} else {
			gx->activeParams = _gxCommandParams[entry.command];
			if (gx->activeParams) {
				--gx->activeParams;
			}
			if (gx->activeParams) {
				gx->activeEntries[0] = entry;
			}
		}

		if (gx->activeParams) {
			continue;
		}

		switch (entry.command) {
		case DS_GX_CMD_MTX_MODE:
			if (entry.params[0] < 4) {
				gx->mtxMode = entry.params[0];
			} else {
				mLOG(DS_GX, GAME_ERROR, "Invalid GX MTX_MODE %02X", entry.params[0]);
			}
			break;
		case DS_GX_CMD_MTX_IDENTITY:
			switch (gx->mtxMode) {
			case 0:
				DSGXMtxIdentity(&gx->projMatrix);
				break;
			case 2:
				DSGXMtxIdentity(&gx->vecMatrix);
				// Fall through
			case 1:
				DSGXMtxIdentity(&gx->posMatrix);
				break;
			case 3:
				DSGXMtxIdentity(&gx->texMatrix);
				break;
			}
			break;
		case DS_GX_CMD_MTX_PUSH:
			switch (gx->mtxMode) {
			case 0:
				memcpy(&gx->projMatrixStack, &gx->projMatrix, sizeof(gx->projMatrix));
				++projMatrixPointer;
				break;
			case 2:
				memcpy(&gx->vecMatrixStack[pvMatrixPointer & 0x1F], &gx->vecMatrix, sizeof(gx->vecMatrix));
				// Fall through
			case 1:
				memcpy(&gx->posMatrixStack[pvMatrixPointer & 0x1F], &gx->posMatrix, sizeof(gx->posMatrix));
				++pvMatrixPointer;
				break;
			case 3:
				mLOG(DS_GX, STUB, "Unimplemented GX MTX_PUSH mode");
				break;
			}
			break;
		case DS_GX_CMD_MTX_POP: {
			int8_t offset = entry.params[0];
			offset <<= 2;
			offset >>= 2;
			switch (gx->mtxMode) {
			case 0:
				projMatrixPointer -= offset;
				memcpy(&gx->projMatrix, &gx->projMatrixStack, sizeof(gx->projMatrix));
				break;
			case 1:
				pvMatrixPointer -= offset;
				memcpy(&gx->posMatrix, &gx->posMatrixStack[pvMatrixPointer & 0x1F], sizeof(gx->posMatrix));
				break;
			case 2:
				pvMatrixPointer -= offset;
				memcpy(&gx->vecMatrix, &gx->vecMatrixStack[pvMatrixPointer & 0x1F], sizeof(gx->vecMatrix));
				memcpy(&gx->posMatrix, &gx->posMatrixStack[pvMatrixPointer & 0x1F], sizeof(gx->posMatrix));
				break;
			case 3:
				mLOG(DS_GX, STUB, "Unimplemented GX MTX_POP mode");
				break;
			}
			break;
		}
		case DS_GX_CMD_SWAP_BUFFERS:
			gx->swapBuffers = true;
			break;
		default:
			mLOG(DS_GX, STUB, "Unimplemented GX command %02X:%02X %02X %02X %02X", entry.command, entry.params[0], entry.params[1], entry.params[2], entry.params[3]);
			break;
		}

		gxstat = DSRegGXSTATSetPVMatrixStackLevel(gxstat, pvMatrixPointer);
		gxstat = DSRegGXSTATSetProjMatrixStackLevel(gxstat, projMatrixPointer);
		gxstat = DSRegGXSTATTestFillMatrixStackError(gxstat, projMatrixPointer || pvMatrixPointer >= 0x1F);
		gx->p->memory.io9[DS9_REG_GXSTAT_LO >> 1] = gxstat;

		if (cyclesLate >= cycles) {
			cyclesLate -= cycles;
		} else {
			break;
		}
	}
	DSGXUpdateGXSTAT(gx);
	if (cycles && !gx->swapBuffers) {
		mTimingSchedule(timing, &gx->fifoEvent, cycles - cyclesLate);
	}
}

void DSGXInit(struct DSGX* gx) {
	gx->renderer = &dummyRenderer;
	CircleBufferInit(&gx->fifo, sizeof(struct DSGXEntry) * DS_GX_FIFO_SIZE);
	CircleBufferInit(&gx->pipe, sizeof(struct DSGXEntry) * DS_GX_PIPE_SIZE);
	gx->fifoEvent.name = "DS GX FIFO";
	gx->fifoEvent.priority = 0xC;
	gx->fifoEvent.context = gx;
	gx->fifoEvent.callback = _fifoRun;
}

void DSGXDeinit(struct DSGX* gx) {
	DSGXAssociateRenderer(gx, &dummyRenderer);
	CircleBufferDeinit(&gx->fifo);
	CircleBufferDeinit(&gx->pipe);
}

void DSGXReset(struct DSGX* gx) {
	CircleBufferClear(&gx->fifo);
	CircleBufferClear(&gx->pipe);
	DSGXMtxIdentity(&gx->projMatrix);
	DSGXMtxIdentity(&gx->texMatrix);
	DSGXMtxIdentity(&gx->posMatrix);
	DSGXMtxIdentity(&gx->vecMatrix);

	DSGXMtxIdentity(&gx->projMatrixStack);
	DSGXMtxIdentity(&gx->texMatrixStack);
	int i;
	for (i = 0; i < 32; ++i) {
		DSGXMtxIdentity(&gx->posMatrixStack[i]);
		DSGXMtxIdentity(&gx->vecMatrixStack[i]);
	}
	gx->swapBuffers = false;
	gx->bufferIndex = 0;
	gx->mtxMode = 0;

	memset(gx->outstandingParams, 0, sizeof(gx->outstandingParams));
	memset(gx->outstandingCommand, 0, sizeof(gx->outstandingCommand));
	gx->activeParams = 0;
}

void DSGXAssociateRenderer(struct DSGX* gx, struct DSGXRenderer* renderer) {
	gx->renderer->deinit(gx->renderer);
	gx->renderer = renderer;
	gx->renderer->init(gx->renderer);
}

void DSGXUpdateGXSTAT(struct DSGX* gx) {
	uint32_t value = gx->p->memory.io9[DS9_REG_GXSTAT_HI >> 1] << 16;
	value = DSRegGXSTATIsDoIRQ(value);

	size_t entries = CircleBufferSize(&gx->fifo) / sizeof(struct DSGXEntry);
	// XXX
	if (gx->swapBuffers) {
		entries++;
	}
	value = DSRegGXSTATSetFIFOEntries(value, entries);
	value = DSRegGXSTATSetFIFOLtHalf(value, entries < (DS_GX_FIFO_SIZE / 2));
	value = DSRegGXSTATSetFIFOEmpty(value, entries == 0);

	if ((DSRegGXSTATGetDoIRQ(value) == 1 && entries < (DS_GX_FIFO_SIZE / 2)) ||
		(DSRegGXSTATGetDoIRQ(value) == 2 && entries == 0)) {
		DSRaiseIRQ(gx->p->ds9.cpu, gx->p->ds9.memory.io, DS_IRQ_GEOM_FIFO);
	}

	value = DSRegGXSTATSetBusy(value, mTimingIsScheduled(&gx->p->ds9.timing, &gx->fifoEvent) || gx->swapBuffers);

	gx->p->memory.io9[DS9_REG_GXSTAT_HI >> 1] = value >> 16;
}

static void DSGXUnpackCommand(struct DSGX* gx, uint32_t command) {
	gx->outstandingCommand[0] = command;
	gx->outstandingCommand[1] = command >> 8;
	gx->outstandingCommand[2] = command >> 16;
	gx->outstandingCommand[3] = command >> 24;
	if (gx->outstandingCommand[0] >= DS_GX_CMD_MAX) {
		gx->outstandingCommand[0] = 0;
	}
	if (gx->outstandingCommand[1] >= DS_GX_CMD_MAX) {
		gx->outstandingCommand[1] = 0;
	}
	if (gx->outstandingCommand[2] >= DS_GX_CMD_MAX) {
		gx->outstandingCommand[2] = 0;
	}
	if (gx->outstandingCommand[3] >= DS_GX_CMD_MAX) {
		gx->outstandingCommand[3] = 0;
	}
	gx->outstandingParams[0] = _gxCommandParams[gx->outstandingCommand[0]];
	gx->outstandingParams[1] = _gxCommandParams[gx->outstandingCommand[1]];
	gx->outstandingParams[2] = _gxCommandParams[gx->outstandingCommand[2]];
	gx->outstandingParams[3] = _gxCommandParams[gx->outstandingCommand[3]];
}

static void DSGXWriteFIFO(struct DSGX* gx, struct DSGXEntry entry) {
	if (gx->outstandingParams[0]) {
		entry.command = gx->outstandingCommand[0];
		--gx->outstandingParams[0];
		if (!gx->outstandingParams[0]) {
			// TODO: improve this
			memmove(&gx->outstandingParams[0], &gx->outstandingParams[1], sizeof(gx->outstandingParams[0]) * 3);
			memmove(&gx->outstandingCommand[0], &gx->outstandingCommand[1], sizeof(gx->outstandingCommand[0]) * 3);
			gx->outstandingParams[3] = 0;
		}
	} else {
		gx->outstandingCommand[0] = entry.command;
		gx->outstandingParams[0] = _gxCommandParams[entry.command];
		if (gx->outstandingParams[0]) {
			--gx->outstandingParams[0];
		}
	}
	uint32_t cycles = _gxCommandCycleBase[entry.command];
	if (!cycles) {
		return;
	}
	if (CircleBufferSize(&gx->fifo) == 0 && CircleBufferSize(&gx->pipe) < (DS_GX_PIPE_SIZE * sizeof(entry))) {
		CircleBufferWrite8(&gx->pipe, entry.command);
		CircleBufferWrite8(&gx->pipe, entry.params[0]);
		CircleBufferWrite8(&gx->pipe, entry.params[1]);
		CircleBufferWrite8(&gx->pipe, entry.params[2]);
		CircleBufferWrite8(&gx->pipe, entry.params[3]);
	} else if (CircleBufferSize(&gx->fifo) < (DS_GX_FIFO_SIZE * sizeof(entry))) {
		CircleBufferWrite8(&gx->fifo, entry.command);
		CircleBufferWrite8(&gx->fifo, entry.params[0]);
		CircleBufferWrite8(&gx->fifo, entry.params[1]);
		CircleBufferWrite8(&gx->fifo, entry.params[2]);
		CircleBufferWrite8(&gx->fifo, entry.params[3]);
	} else {
		mLOG(DS_GX, STUB, "Unimplemented GX full");
	}
	if (!gx->swapBuffers && !mTimingIsScheduled(&gx->p->ds9.timing, &gx->fifoEvent)) {
		mTimingSchedule(&gx->p->ds9.timing, &gx->fifoEvent, cycles);
	}
}

uint16_t DSGXWriteRegister(struct DSGX* gx, uint32_t address, uint16_t value) {
	uint16_t oldValue = gx->p->memory.io9[address >> 1];
	switch (address) {
	case DS9_REG_DISP3DCNT:
		mLOG(DS_GX, STUB, "Unimplemented GX write %03X:%04X", address, value);
		break;
	case DS9_REG_GXSTAT_LO:
		value = DSRegGXSTATIsMatrixStackError(value);
		if (value) {
			oldValue = DSRegGXSTATClearMatrixStackError(oldValue);
			oldValue = DSRegGXSTATClearProjMatrixStackLevel(oldValue);
		}
		value = oldValue;
		break;
	case DS9_REG_GXSTAT_HI:
		value = DSRegGXSTATIsDoIRQ(value << 16) >> 16;
		gx->p->memory.io9[address >> 1] = value;
		DSGXUpdateGXSTAT(gx);
		value = gx->p->memory.io9[address >> 1];
		break;
	default:
		if (address < DS9_REG_GXFIFO_00) {
			mLOG(DS_GX, STUB, "Unimplemented GX write %03X:%04X", address, value);
		} else if (address <= DS9_REG_GXFIFO_1F) {
			mLOG(DS_GX, STUB, "Unimplemented GX write %03X:%04X", address, value);
		} else if (address < DS9_REG_GXSTAT_LO) {
			struct DSGXEntry entry = {
				.command = (address & 0x1FC) >> 2,
				.params = {
					value,
					value >> 8,
				}
			};
			if (entry.command < DS_GX_CMD_MAX) {
				DSGXWriteFIFO(gx, entry);
			}
		} else {
			mLOG(DS_GX, STUB, "Unimplemented GX write %03X:%04X", address, value);
		}
		break;
	}
	return value;
}

uint32_t DSGXWriteRegister32(struct DSGX* gx, uint32_t address, uint32_t value) {
	switch (address) {
	case DS9_REG_DISP3DCNT:
		mLOG(DS_GX, STUB, "Unimplemented GX write %03X:%08X", address, value);
		break;
	case DS9_REG_GXSTAT_LO:
		value = (value & 0xFFFF0000) | DSGXWriteRegister(gx, DS9_REG_GXSTAT_LO, value);
		value = (value & 0x0000FFFF) | (DSGXWriteRegister(gx, DS9_REG_GXSTAT_HI, value >> 16) << 16);
		break;
	default:
		if (address < DS9_REG_GXFIFO_00) {
			mLOG(DS_GX, STUB, "Unimplemented GX write %03X:%08X", address, value);
		} else if (address <= DS9_REG_GXFIFO_1F) {
			if (gx->outstandingParams[0]) {
				struct DSGXEntry entry = {
					.command = gx->outstandingCommand[0],
					.params = {
						value,
						value >> 8,
						value >> 16,
						value >> 24
					}
				};
				DSGXWriteFIFO(gx, entry);
			} else {
				DSGXUnpackCommand(gx, value);
			}
		} else if (address < DS9_REG_GXSTAT_LO) {
			struct DSGXEntry entry = {
				.command = (address & 0x1FC) >> 2,
				.params = {
					value,
					value >> 8,
					value >> 16,
					value >> 24
				}
			};
			DSGXWriteFIFO(gx, entry);
		} else {
			mLOG(DS_GX, STUB, "Unimplemented GX write %03X:%08X", address, value);
		}
		break;
	}
	return value;
}

void DSGXSwapBuffers(struct DSGX* gx) {
	mLOG(DS_GX, STUB, "Unimplemented GX swap buffers");
	gx->swapBuffers = false;

	// TODO
	DSGXUpdateGXSTAT(gx);
	if (CircleBufferSize(&gx->fifo)) {
		mTimingSchedule(&gx->p->ds9.timing, &gx->fifoEvent, 0);
	}
}

static void DSGXDummyRendererInit(struct DSGXRenderer* renderer) {
	UNUSED(renderer);
	// Nothing to do
}

static void DSGXDummyRendererReset(struct DSGXRenderer* renderer) {
	UNUSED(renderer);
	// Nothing to do
}

static void DSGXDummyRendererDeinit(struct DSGXRenderer* renderer) {
	UNUSED(renderer);
	// Nothing to do
}

static void DSGXDummyRendererSetRAM(struct DSGXRenderer* renderer, struct DSGXVertex* verts, struct DSGXPolygon* polys, unsigned polyCount) {
	UNUSED(renderer);
	UNUSED(verts);
	UNUSED(polys);
	UNUSED(polyCount);
	// Nothing to do
}

static void DSGXDummyRendererDrawScanline(struct DSGXRenderer* renderer, int y) {
	UNUSED(renderer);
	UNUSED(y);
	// Nothing to do
}

static void DSGXDummyRendererGetScanline(struct DSGXRenderer* renderer, int y, color_t** output) {
	UNUSED(renderer);
	UNUSED(y);
	*output = NULL;
	// Nothing to do
}