#include "u8bidi.hpp"
#include "u8bidi_impl.hpp"

#include <cstring>

/*
 * setTrailingWSStart() sets the start index for a trailing
 * run of WS in the line. This is necessary because we do not modify
 * the paragraph's levels array that we just point into.
 * Using trailingWSStart is another form of performing (L1).
 *
 * To make subsequent operations easier, we also include the run
 * before the WS if it is at the paraLevel - we merge the two here.
 *
 * This function is called only from ubidi_setLine(), so pBiDi->paraLevel is
 * set correctly for the line even when contextual multiple paragraphs.
 */
static void setTrailingWSStart(U8BiDi* pBiDi);

/* in trivial cases there is only one trivial run; called by ubidi_getRuns() */
static void getSingleRun(U8BiDi *pBiDi, UBiDiLevel level);

/*
 * Reorder the same-level runs in the runs array.
 * Here, runCount>1 and maxLevel>=minLevel>=paraLevel.
 * All the visualStart fields=logical start before reordering.
 * The "odd" bits are not set yet.
 *
 * Reordering with this data structure lends itself to some handy shortcuts:
 *
 * Since each run is moved but not modified, and since at the initial maxLevel
 * each sequence of same-level runs consists of only one run each, we
 * don't need to do anything there and can predecrement maxLevel.
 * In many simple cases, the reordering is thus done entirely in the
 * index mapping.
 * Also, reordering occurs only down to the lowest odd level that occurs,
 * which is minLevel|1. However, if the lowest level itself is odd, then
 * in the last reordering the sequence of the runs at this level or higher
 * will be all runs, and we don't need the elaborate loop to search for them.
 * This is covered by ++minLevel instead of minLevel|=1 followed
 * by an extra reorder-all after the reorder-some loop.
 * About a trailing WS run:
 * Such a run would need special treatment because its level is not
 * reflected in levels[] if this is not a paragraph object.
 * Instead, all characters from trailingWSStart on are implicitly at
 * paraLevel.
 * However, for all maxLevel>paraLevel, this run will never be reordered
 * and does not need to be taken into account. maxLevel==paraLevel is only reordered
 * if minLevel==paraLevel is odd, which is done in the extra segment.
 * This means that for the main reordering loop we don't need to consider
 * this run and can --runCount. If it is later part of the all-runs
 * reordering, then runCount is adjusted accordingly.
 */
static void reorderLine(U8BiDi* pBiDi, UBiDiLevel minLevel, UBiDiLevel maxLevel);

static int32_t getRunFromLogicalIndex(U8BiDi *pBiDi, int32_t logicalIndex);

// Public Functions

void u8bidi_set_line(const U8BiDi* pParaBiDi, int32_t start, int32_t limit, U8BiDi* pLineBiDi,
		UErrorCode* pErrorCode) {
    int32_t length;

    /* check the argument values */
    RETURN_VOID_IF_NULL_OR_FAILING_ERRCODE(pErrorCode);
    RETURN_VOID_IF_NOT_VALID_PARA(pParaBiDi, *pErrorCode);
    RETURN_VOID_IF_BAD_RANGE(start, 0, limit, *pErrorCode);
    RETURN_VOID_IF_BAD_RANGE(limit, 0, pParaBiDi->length+1, *pErrorCode);
    if(pLineBiDi==nullptr) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }
    if(u8bidi_get_paragraph(pParaBiDi, start, nullptr, nullptr, nullptr, pErrorCode) !=
       u8bidi_get_paragraph(pParaBiDi, limit-1, nullptr, nullptr, nullptr, pErrorCode)) {
        /* the line crosses a paragraph boundary */
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }

    /* set the values in pLineBiDi from its pParaBiDi parent */
    pLineBiDi->pParaBiDi=nullptr;          /* mark unfinished setLine */
    pLineBiDi->text=pParaBiDi->text+start;
    length=pLineBiDi->length=limit-start;
    pLineBiDi->resultLength=pLineBiDi->originalLength=length;
    pLineBiDi->paraLevel=get_para_level_internal(pParaBiDi, start);
    pLineBiDi->paraCount=pParaBiDi->paraCount;
    pLineBiDi->runs=nullptr;
    pLineBiDi->flags=0;
    pLineBiDi->reorderingMode=pParaBiDi->reorderingMode;
    pLineBiDi->reorderingOptions=pParaBiDi->reorderingOptions;
    pLineBiDi->controlCount=0;
    if(pParaBiDi->controlCount>0) {
        int32_t j;
        for(j=start; j<limit; j++) {
			// FIXME: FIXME: FIXME: UTF-8 logic warning
            if(IS_BIDI_CONTROL_CHAR(pParaBiDi->text[j])) {
                pLineBiDi->controlCount++;
            }
        }
        pLineBiDi->resultLength-=pLineBiDi->controlCount;
    }

    pLineBiDi->dirProps=pParaBiDi->dirProps+start;
    pLineBiDi->levels=pParaBiDi->levels+start;
    pLineBiDi->runCount=-1;

    if(pParaBiDi->direction!=UBIDI_MIXED) {
        /* the parent is already trivial */
        pLineBiDi->direction=pParaBiDi->direction;

        /*
         * The parent's levels are all either
         * implicitly or explicitly ==paraLevel;
         * do the same here.
         */
        if(pParaBiDi->trailingWSStart<=start) {
            pLineBiDi->trailingWSStart=0;
        } else if(pParaBiDi->trailingWSStart<limit) {
            pLineBiDi->trailingWSStart=pParaBiDi->trailingWSStart-start;
        } else {
            pLineBiDi->trailingWSStart=length;
        }
    } else {
        const UBiDiLevel *levels=pLineBiDi->levels;
        int32_t i, trailingWSStart;
        UBiDiLevel level;

        setTrailingWSStart(pLineBiDi);
        trailingWSStart=pLineBiDi->trailingWSStart;

        /* recalculate pLineBiDi->direction */
        if(trailingWSStart==0) {
            /* all levels are at paraLevel */
            pLineBiDi->direction=(UBiDiDirection)(pLineBiDi->paraLevel&1);
        } else {
            /* get the level of the first character */
            level=(UBiDiLevel)(levels[0]&1);

            /* if there is anything of a different level, then the line is mixed */
            if(trailingWSStart<length && (pLineBiDi->paraLevel&1)!=level) {
                /* the trailing WS is at paraLevel, which differs from levels[0] */
                pLineBiDi->direction=UBIDI_MIXED;
            } else {
                /* see if levels[1..trailingWSStart-1] have the same direction as levels[0] and paraLevel */
                i=1;
                for(;;) {
                    if(i==trailingWSStart) {
                        /* the direction values match those in level */
                        pLineBiDi->direction=(UBiDiDirection)level;
                        break;
                    } else if((levels[i]&1)!=level) {
                        pLineBiDi->direction=UBIDI_MIXED;
                        break;
                    }
                    ++i;
                }
            }
        }

        switch(pLineBiDi->direction) {
        case UBIDI_LTR:
            /* make sure paraLevel is even */
            pLineBiDi->paraLevel=(UBiDiLevel)((pLineBiDi->paraLevel+1)&~1);

            /* all levels are implicitly at paraLevel (important for ubidi_getLevels()) */
            pLineBiDi->trailingWSStart=0;
            break;
        case UBIDI_RTL:
            /* make sure paraLevel is odd */
            pLineBiDi->paraLevel|=1;

            /* all levels are implicitly at paraLevel (important for ubidi_getLevels()) */
            pLineBiDi->trailingWSStart=0;
            break;
        default:
            break;
        }
    }
    pLineBiDi->pParaBiDi=pParaBiDi;     /* mark successful setLine */
}

UBiDiLevel u8bidi_get_level_at(const U8BiDi* pBiDi, int32_t charIndex) {
	/* return paraLevel if in the trailing WS run, otherwise the real level */
    if(!IS_VALID_PARA_OR_LINE(pBiDi) || charIndex<0 || pBiDi->length<=charIndex) {
        return 0;
    } else if(pBiDi->direction!=UBIDI_MIXED || charIndex>=pBiDi->trailingWSStart) {
        return get_para_level_internal(pBiDi, charIndex);
    } else {
        return pBiDi->levels[charIndex];
    }
}

void u8bidi_get_logical_run(const U8BiDi* pBiDi, int32_t logicalPosition, int32_t* pLogicalLimit,
		UBiDiLevel* pLevel) {
	UErrorCode errorCode;
    int32_t runCount, visualStart, logicalLimit, logicalFirst, i;
    Run iRun;

    errorCode=U_ZERO_ERROR;
    RETURN_VOID_IF_BAD_RANGE(logicalPosition, 0, pBiDi->length, errorCode);
    /* ubidi_countRuns will check VALID_PARA_OR_LINE */
    runCount=ubidi_countRuns((UBiDi *)pBiDi, &errorCode);
    if(U_FAILURE(errorCode)) {
        return;
    }
    /* this is done based on runs rather than on levels since levels have
       a special interpretation when UBIDI_REORDER_RUNS_ONLY
     */
    visualStart=logicalLimit=0;
    iRun=pBiDi->runs[0];

    for(i=0; i<runCount; i++) {
        iRun = pBiDi->runs[i];
        logicalFirst=GET_INDEX(iRun.logicalStart);
        logicalLimit=logicalFirst+iRun.visualLimit-visualStart;
        if((logicalPosition>=logicalFirst) &&
           (logicalPosition<logicalLimit)) {
            break;
        }
        visualStart = iRun.visualLimit;
    }
    if(pLogicalLimit) {
        *pLogicalLimit=logicalLimit;
    }
    if(pLevel) {
        if(pBiDi->reorderingMode==UBIDI_REORDER_RUNS_ONLY) {
            *pLevel=(UBiDiLevel)GET_ODD_BIT(iRun.logicalStart);
        }
        else if(pBiDi->direction!=UBIDI_MIXED || logicalPosition>=pBiDi->trailingWSStart) {
            *pLevel=get_para_level_internal(pBiDi, logicalPosition);
        } else {
        *pLevel=pBiDi->levels[logicalPosition];
        }
    }
}

int32_t u8bidi_count_runs(U8BiDi* pBiDi, UErrorCode* pErrorCode) {
	RETURN_IF_NULL_OR_FAILING_ERRCODE(pErrorCode, -1);
    RETURN_IF_NOT_VALID_PARA_OR_LINE(pBiDi, *pErrorCode, -1);
    u8bidi_get_runs(pBiDi, pErrorCode);
    if(U_FAILURE(*pErrorCode)) {
        return -1;
    }
    return pBiDi->runCount;
}

UBiDiDirection u8bidi_get_visual_run(U8BiDi* pBiDi, int32_t runIndex, int32_t* pLogicalStart, int32_t* pLength) {
    int32_t start;
    UErrorCode errorCode = U_ZERO_ERROR;
    RETURN_IF_NOT_VALID_PARA_OR_LINE(pBiDi, errorCode, UBIDI_LTR);
    u8bidi_get_runs(pBiDi, &errorCode);
    if(U_FAILURE(errorCode)) {
        return UBIDI_LTR;
    }
    RETURN_IF_BAD_RANGE(runIndex, 0, pBiDi->runCount, errorCode, UBIDI_LTR);

    start=pBiDi->runs[runIndex].logicalStart;
    if(pLogicalStart!=nullptr) {
        *pLogicalStart=GET_INDEX(start);
    }
    if(pLength!=nullptr) {
        if(runIndex>0) {
            *pLength=pBiDi->runs[runIndex].visualLimit-
                     pBiDi->runs[runIndex-1].visualLimit;
        } else {
            *pLength=pBiDi->runs[0].visualLimit;
        }
    }
    return (UBiDiDirection)GET_ODD_BIT(start);
}

const UBiDiLevel* u8bidi_get_levels(U8BiDi* pBiDi, UErrorCode* pErrorCode) {
    int32_t start, length;

    RETURN_IF_NULL_OR_FAILING_ERRCODE(pErrorCode, nullptr);
    RETURN_IF_NOT_VALID_PARA_OR_LINE(pBiDi, *pErrorCode, nullptr);
    if((length=pBiDi->length)<=0) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return nullptr;
    }
    if((start=pBiDi->trailingWSStart)==length) {
        /* the current levels array reflects the WS run */
        return pBiDi->levels;
    }

    /*
     * After the previous if(), we know that the levels array
     * has an implicit trailing WS run and therefore does not fully
     * reflect itself all the levels.
     * This must be a UBiDi object for a line, and
     * we need to create a new levels array.
     */
    if(getLevelsMemory(pBiDi, length)) {
        UBiDiLevel *levels=pBiDi->levelsMemory;

        if(start>0 && levels!=pBiDi->levels) {
			std::memcpy(levels, pBiDi->levels, start);
        }
        /* pBiDi->paraLevel is ok even if contextual multiple paragraphs,
           since pBidi is a line object                                     */
		std::memset(levels+start, pBiDi->paraLevel, length-start);

        /* this new levels array is set for the line and reflects the WS run */
        pBiDi->trailingWSStart=length;
        return pBiDi->levels=levels;
    } else {
        /* out of memory */
        *pErrorCode=U_MEMORY_ALLOCATION_ERROR;
        return nullptr;
    }
}

int32_t u8bidi_get_visual_index(U8BiDi* pBiDi, int32_t logicalIndex, UErrorCode* pErrorCode) {
	int32_t visualIndex=UBIDI_MAP_NOWHERE;
    RETURN_IF_NULL_OR_FAILING_ERRCODE(pErrorCode, -1);
    RETURN_IF_NOT_VALID_PARA_OR_LINE(pBiDi, *pErrorCode, -1);
    RETURN_IF_BAD_RANGE(logicalIndex, 0, pBiDi->length, *pErrorCode, -1);

    /* we can do the trivial cases without the runs array */
    switch(pBiDi->direction) {
    case UBIDI_LTR:
        visualIndex=logicalIndex;
        break;
    case UBIDI_RTL:
        visualIndex=pBiDi->length-logicalIndex-1;
        break;
    default:
        if(!u8bidi_get_runs(pBiDi, pErrorCode)) {
            *pErrorCode=U_MEMORY_ALLOCATION_ERROR;
            return -1;
        } else {
            Run *runs=pBiDi->runs;
            int32_t i, visualStart=0, offset, length;

            /* linear search for the run, search on the visual runs */
            for(i=0; i<pBiDi->runCount; ++i) {
                length=runs[i].visualLimit-visualStart;
                offset=logicalIndex-GET_INDEX(runs[i].logicalStart);
                if(offset>=0 && offset<length) {
                    if(IS_EVEN_RUN(runs[i].logicalStart)) {
                        /* LTR */
                        visualIndex=visualStart+offset;
                    } else {
                        /* RTL */
                        visualIndex=visualStart+length-offset-1;
                    }
                    break;          /* exit for loop */
                }
                visualStart+=length;
            }
            if(i>=pBiDi->runCount) {
                return UBIDI_MAP_NOWHERE;
            }
        }
    }

    if(pBiDi->insertPoints.size>0) {
        /* add the number of added marks until the calculated visual index */
        Run *runs=pBiDi->runs;
        int32_t i, length, insertRemove;
        int32_t visualStart=0, markFound=0;
        for(i=0; ; i++, visualStart+=length) {
            length=runs[i].visualLimit-visualStart;
            insertRemove=runs[i].insertRemove;
            if(insertRemove & (LRM_BEFORE|RLM_BEFORE)) {
                markFound++;
            }
            /* is it the run containing the visual index? */
            if(visualIndex<runs[i].visualLimit) {
                return visualIndex+markFound;
            }
            if(insertRemove & (LRM_AFTER|RLM_AFTER)) {
                markFound++;
            }
        }
    }
    else if(pBiDi->controlCount>0) {
        /* subtract the number of controls until the calculated visual index */
        Run *runs=pBiDi->runs;
        int32_t i, j, start, limit, length, insertRemove;
        int32_t visualStart=0, controlFound=0;
		// FIXME: FIXME: FIXME: UTF-8 Encoding Issue!!!!!
        char16_t uchar=pBiDi->text[logicalIndex];
        /* is the logical index pointing to a control ? */
		// FIXME: FIXME: FIXME: UTF-8 Encoding Issue!!!!!
        if(IS_BIDI_CONTROL_CHAR(uchar)) {
            return UBIDI_MAP_NOWHERE;
        }
        /* loop on runs */
        for(i=0; ; i++, visualStart+=length) {
            length=runs[i].visualLimit-visualStart;
            insertRemove=runs[i].insertRemove;
            /* calculated visual index is beyond this run? */
            if(visualIndex>=runs[i].visualLimit) {
                controlFound-=insertRemove;
                continue;
            }
            /* calculated visual index must be within current run */
            if(insertRemove==0) {
                return visualIndex-controlFound;
            }
            if(IS_EVEN_RUN(runs[i].logicalStart)) {
                /* LTR: check from run start to logical index */
                start=runs[i].logicalStart;
                limit=logicalIndex;
            } else {
                /* RTL: check from logical index to run end */
                start=logicalIndex+1;
                limit=GET_INDEX(runs[i].logicalStart)+length;
            }
            for(j=start; j<limit; j++) {
                uchar=pBiDi->text[j];
				// FIXME: FIXME: FIXME: UTF-8 Encoding Issue!!!!!
                if(IS_BIDI_CONTROL_CHAR(uchar)) {
                    controlFound++;
                }
            }
            return visualIndex-controlFound;
        }
    }

    return visualIndex;
}

int32_t u8bidi_get_logical_index(U8BiDi* pBiDi, int32_t visualIndex, UErrorCode* pErrorCode) {
	Run *runs;
    int32_t i, runCount, start;
    RETURN_IF_NULL_OR_FAILING_ERRCODE(pErrorCode, -1);
    RETURN_IF_NOT_VALID_PARA_OR_LINE(pBiDi, *pErrorCode, -1);
    RETURN_IF_BAD_RANGE(visualIndex, 0, pBiDi->resultLength, *pErrorCode, -1);
    /* we can do the trivial cases without the runs array */
    if(pBiDi->insertPoints.size==0 && pBiDi->controlCount==0) {
        if(pBiDi->direction==UBIDI_LTR) {
            return visualIndex;
        }
        else if(pBiDi->direction==UBIDI_RTL) {
            return pBiDi->length-visualIndex-1;
        }
    }
    if(!u8bidi_get_runs(pBiDi, pErrorCode)) {
        *pErrorCode=U_MEMORY_ALLOCATION_ERROR;
        return -1;
    }

    runs=pBiDi->runs;
    runCount=pBiDi->runCount;
    if(pBiDi->insertPoints.size>0) {
        /* handle inserted LRM/RLM */
        int32_t markFound=0, insertRemove;
        int32_t visualStart=0, length;
        runs=pBiDi->runs;
        /* subtract number of marks until visual index */
        for(i=0; ; i++, visualStart+=length) {
            length=runs[i].visualLimit-visualStart;
            insertRemove=runs[i].insertRemove;
            if(insertRemove&(LRM_BEFORE|RLM_BEFORE)) {
                if(visualIndex<=(visualStart+markFound)) {
                    return UBIDI_MAP_NOWHERE;
                }
                markFound++;
            }
            /* is adjusted visual index within this run? */
            if(visualIndex<(runs[i].visualLimit+markFound)) {
                visualIndex-=markFound;
                break;
            }
            if(insertRemove&(LRM_AFTER|RLM_AFTER)) {
                if(visualIndex==(visualStart+length+markFound)) {
                    return UBIDI_MAP_NOWHERE;
                }
                markFound++;
            }
        }
    }
    else if(pBiDi->controlCount>0) {
        /* handle removed BiDi control characters */
        int32_t controlFound=0, insertRemove, length;
        int32_t logicalStart, logicalEnd, visualStart=0, j, k;
        char16_t uchar; // FIXME: !!!!!!!!!!!
        UBool evenRun;
        /* add number of controls until visual index */
        for(i=0; ; i++, visualStart+=length) {
            length=runs[i].visualLimit-visualStart;
            insertRemove=runs[i].insertRemove;
            /* is adjusted visual index beyond current run? */
            if(visualIndex>=(runs[i].visualLimit-controlFound+insertRemove)) {
                controlFound-=insertRemove;
                continue;
            }
            /* adjusted visual index is within current run */
            if(insertRemove==0) {
                visualIndex+=controlFound;
                break;
            }
            /* count non-control chars until visualIndex */
            logicalStart=runs[i].logicalStart;
            evenRun=IS_EVEN_RUN(logicalStart);
            REMOVE_ODD_BIT(logicalStart);
            logicalEnd=logicalStart+length-1;
            for(j=0; j<length; j++) {
                k= evenRun ? logicalStart+j : logicalEnd-j;
				// FIXME: FIXME: FIXME: Unicode Encoding issue!!!!!!!!
                uchar=pBiDi->text[k];
                if(IS_BIDI_CONTROL_CHAR(uchar)) {
                    controlFound++;
                }
                if((visualIndex+controlFound)==(visualStart+j)) {
                    break;
                }
            }
            visualIndex+=controlFound;
            break;
        }
    }
    /* handle all cases */
    if(runCount<=10) {
        /* linear search for the run */
        for(i=0; visualIndex>=runs[i].visualLimit; ++i) {}
    } else {
        /* binary search for the run */
        int32_t begin=0, limit=runCount;

        /* the middle if() is guaranteed to find the run, we don't need a loop limit */
        for(;;) {
            i=(begin+limit)/2;
            if(visualIndex>=runs[i].visualLimit) {
                begin=i+1;
            } else if(i==0 || visualIndex>=runs[i-1].visualLimit) {
                break;
            } else {
                limit=i;
            }
        }
    }

    start=runs[i].logicalStart;
    if(IS_EVEN_RUN(start)) {
        /* LTR */
        /* the offset in runs[i] is visualIndex-runs[i-1].visualLimit */
        if(i>0) {
            visualIndex-=runs[i-1].visualLimit;
        }
        return start+visualIndex;
    } else {
        /* RTL */
        return GET_INDEX(start)+runs[i].visualLimit-visualIndex-1;
    }
}

void u8bidi_get_logical_map(U8BiDi* pBiDi, int32_t* indexMap, UErrorCode* pErrorCode) {
	RETURN_VOID_IF_NULL_OR_FAILING_ERRCODE(pErrorCode);
    /* ubidi_countRuns() checks for VALID_PARA_OR_LINE */
    u8bidi_count_runs(pBiDi, pErrorCode);
    if(U_FAILURE(*pErrorCode)) {
        /* no op */
    } else if(indexMap==nullptr) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
    } else {
        /* fill a logical-to-visual index map using the runs[] */
        int32_t visualStart, visualLimit, i, j, k;
        int32_t logicalStart, logicalLimit;
        Run *runs=pBiDi->runs;
        if (pBiDi->length<=0) {
            return;
        }

        if (pBiDi->length>pBiDi->resultLength) {
			std::memset(indexMap, 0xFF, pBiDi->length * sizeof(int32_t));
        }

        visualStart=0;
        for(j=0; j<pBiDi->runCount; ++j) {
            logicalStart=GET_INDEX(runs[j].logicalStart);
            visualLimit=runs[j].visualLimit;
            if(IS_EVEN_RUN(runs[j].logicalStart)) {
                do { /* LTR */
                    indexMap[logicalStart++]=visualStart++;
                } while(visualStart<visualLimit);
            } else {
                logicalStart+=visualLimit-visualStart;  /* logicalLimit */
                do { /* RTL */
                    indexMap[--logicalStart]=visualStart++;
                } while(visualStart<visualLimit);
            }
            /* visualStart==visualLimit; */
        }

        if(pBiDi->insertPoints.size>0) {
            int32_t markFound=0, runCount=pBiDi->runCount;
            int32_t length, insertRemove;
            visualStart=0;
            /* add number of marks found until each index */
            for(i=0; i<runCount; i++, visualStart+=length) {
                length=runs[i].visualLimit-visualStart;
                insertRemove=runs[i].insertRemove;
                if(insertRemove&(LRM_BEFORE|RLM_BEFORE)) {
                    markFound++;
                }
                if(markFound>0) {
                    logicalStart=GET_INDEX(runs[i].logicalStart);
                    logicalLimit=logicalStart+length;
                    for(j=logicalStart; j<logicalLimit; j++) {
                        indexMap[j]+=markFound;
                    }
                }
                if(insertRemove&(LRM_AFTER|RLM_AFTER)) {
                    markFound++;
                }
            }
        }
        else if(pBiDi->controlCount>0) {
            int32_t controlFound=0, runCount=pBiDi->runCount;
            int32_t length, insertRemove;
            UBool evenRun;
            char16_t uchar;
            visualStart=0;
            /* subtract number of controls found until each index */
            for(i=0; i<runCount; i++, visualStart+=length) {
                length=runs[i].visualLimit-visualStart;
                insertRemove=runs[i].insertRemove;
                /* no control found within previous runs nor within this run */
                if((controlFound-insertRemove)==0) {
                    continue;
                }
                logicalStart=runs[i].logicalStart;
                evenRun=IS_EVEN_RUN(logicalStart);
                REMOVE_ODD_BIT(logicalStart);
                logicalLimit=logicalStart+length;
                /* if no control within this run */
                if(insertRemove==0) {
                    for(j=logicalStart; j<logicalLimit; j++) {
                        indexMap[j]-=controlFound;
                    }
                    continue;
                }
                for(j=0; j<length; j++) {
                    k= evenRun ? logicalStart+j : logicalLimit-j-1;
					// FIXME: FIXME: FIXME: Unicode encoding issue!!!!
                    uchar=pBiDi->text[k];
                    if(IS_BIDI_CONTROL_CHAR(uchar)) {
                        controlFound++;
                        indexMap[k]=UBIDI_MAP_NOWHERE;
                        continue;
                    }
                    indexMap[k]-=controlFound;
                }
            }
        }
    }
}

void u8bidi_get_visual_map(U8BiDi* pBiDi, int32_t* indexMap, UErrorCode* pErrorCode) {
    RETURN_VOID_IF_NULL_OR_FAILING_ERRCODE(pErrorCode);
    if(indexMap==nullptr) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }
    /* ubidi_countRuns() checks for VALID_PARA_OR_LINE */
    u8bidi_count_runs(pBiDi, pErrorCode);
    if(U_SUCCESS(*pErrorCode)) {
        /* fill a visual-to-logical index map using the runs[] */
        Run *runs=pBiDi->runs, *runsLimit=runs+pBiDi->runCount;
        int32_t logicalStart, visualStart, visualLimit, *pi=indexMap;

        if (pBiDi->resultLength<=0) {
            return;
        }
        visualStart=0;
        for(; runs<runsLimit; ++runs) {
            logicalStart=runs->logicalStart;
            visualLimit=runs->visualLimit;
            if(IS_EVEN_RUN(logicalStart)) {
                do { /* LTR */
                    *pi++ = logicalStart++;
                } while(++visualStart<visualLimit);
            } else {
                REMOVE_ODD_BIT(logicalStart);
                logicalStart+=visualLimit-visualStart;  /* logicalLimit */
                do { /* RTL */
                    *pi++ = --logicalStart;
                } while(++visualStart<visualLimit);
            }
            /* visualStart==visualLimit; */
        }

        if(pBiDi->insertPoints.size>0) {
            int32_t markFound=0, runCount=pBiDi->runCount;
            int32_t insertRemove, i, j, k;
            runs=pBiDi->runs;
            /* count all inserted marks */
            for(i=0; i<runCount; i++) {
                insertRemove=runs[i].insertRemove;
                if(insertRemove&(LRM_BEFORE|RLM_BEFORE)) {
                    markFound++;
                }
                if(insertRemove&(LRM_AFTER|RLM_AFTER)) {
                    markFound++;
                }
            }
            /* move back indexes by number of preceding marks */
            k=pBiDi->resultLength;
            for(i=runCount-1; i>=0 && markFound>0; i--) {
                insertRemove=runs[i].insertRemove;
                if(insertRemove&(LRM_AFTER|RLM_AFTER)) {
                    indexMap[--k]= UBIDI_MAP_NOWHERE;
                    markFound--;
                }
                visualStart= i>0 ? runs[i-1].visualLimit : 0;
                for(j=runs[i].visualLimit-1; j>=visualStart && markFound>0; j--) {
                    indexMap[--k]=indexMap[j];
                }
                if(insertRemove&(LRM_BEFORE|RLM_BEFORE)) {
                    indexMap[--k]= UBIDI_MAP_NOWHERE;
                    markFound--;
                }
            }
        }
        else if(pBiDi->controlCount>0) {
            int32_t runCount=pBiDi->runCount, logicalEnd;
            int32_t insertRemove, length, i, j, k, m;
            char16_t uchar;
            UBool evenRun;
            runs=pBiDi->runs;
            visualStart=0;
            /* move forward indexes by number of preceding controls */
            k=0;
            for(i=0; i<runCount; i++, visualStart+=length) {
                length=runs[i].visualLimit-visualStart;
                insertRemove=runs[i].insertRemove;
                /* if no control found yet, nothing to do in this run */
                if((insertRemove==0)&&(k==visualStart)) {
                    k+=length;
                    continue;
                }
                /* if no control in this run */
                if(insertRemove==0) {
                    visualLimit=runs[i].visualLimit;
                    for(j=visualStart; j<visualLimit; j++) {
                        indexMap[k++]=indexMap[j];
                    }
                    continue;
                }
                logicalStart=runs[i].logicalStart;
                evenRun=IS_EVEN_RUN(logicalStart);
                REMOVE_ODD_BIT(logicalStart);
                logicalEnd=logicalStart+length-1;
                for(j=0; j<length; j++) {
                    m= evenRun ? logicalStart+j : logicalEnd-j;
					// FIXME: FIXME: FIXME: Unicode encoding issue!!!!
                    uchar=pBiDi->text[m];
                    if(!IS_BIDI_CONTROL_CHAR(uchar)) {
                        indexMap[k++]=m;
                    }
                }
            }
        }
    }
}

UBool u8bidi_get_runs(U8BiDi* pBiDi, UErrorCode*) {
	/*
     * This method returns immediately if the runs are already set. This
     * includes the case of length==0 (handled in setPara)..
     */
    if (pBiDi->runCount>=0) {
        return true;
    }

    if(pBiDi->direction!=UBIDI_MIXED) {
        /* simple, single-run case - this covers length==0 */
        /* pBiDi->paraLevel is ok even for contextual multiple paragraphs */
        getSingleRun(pBiDi, pBiDi->paraLevel);
    } else /* UBIDI_MIXED, length>0 */ {
        /* mixed directionality */
        int32_t length=pBiDi->length, limit;
        UBiDiLevel *levels=pBiDi->levels;
        int32_t i, runCount;
        UBiDiLevel level=UBIDI_DEFAULT_LTR;   /* initialize with no valid level */
        /*
         * If there are WS characters at the end of the line
         * and the run preceding them has a level different from
         * paraLevel, then they will form their own run at paraLevel (L1).
         * Count them separately.
         * We need some special treatment for this in order to not
         * modify the levels array which a line UBiDi object shares
         * with its paragraph parent and its other line siblings.
         * In other words, for the trailing WS, it may be
         * levels[]!=paraLevel but we have to treat it like it were so.
         */
        limit=pBiDi->trailingWSStart;
        /* count the runs, there is at least one non-WS run, and limit>0 */
        runCount=0;
        for(i=0; i<limit; ++i) {
            /* increment runCount at the start of each run */
            if(levels[i]!=level) {
                ++runCount;
                level=levels[i];
            }
        }

        /*
         * We don't need to see if the last run can be merged with a trailing
         * WS run because setTrailingWSStart() would have done that.
         */
        if(runCount==1 && limit==length) {
            /* There is only one non-WS run and no trailing WS-run. */
            getSingleRun(pBiDi, levels[0]);
        } else /* runCount>1 || limit<length */ {
            /* allocate and set the runs */
            Run *runs;
            int32_t runIndex, start;
            UBiDiLevel minLevel=UBIDI_MAX_EXPLICIT_LEVEL+1, maxLevel=0;

            /* now, count a (non-mergeable) WS run */
            if(limit<length) {
                ++runCount;
            }

            /* runCount>1 */
            if(getRunsMemory(pBiDi, runCount)) {
                runs=pBiDi->runsMemory;
            } else {
                return false;
            }

            /* set the runs */
            /* FOOD FOR THOUGHT: this could be optimized, e.g.:
             * 464->444, 484->444, 575->555, 595->555
             * However, that would take longer. Check also how it would
             * interact with BiDi control removal and inserting Marks.
             */
            runIndex=0;

            /* search for the run limits and initialize visualLimit values with the run lengths */
            i=0;
            do {
                /* prepare this run */
                start=i;
                level=levels[i];
                if(level<minLevel) {
                    minLevel=level;
                }
                if(level>maxLevel) {
                    maxLevel=level;
                }

                /* look for the run limit */
                while(++i<limit && levels[i]==level) {}

                /* i is another run limit */
                runs[runIndex].logicalStart=start;
                runs[runIndex].visualLimit=i-start;
                runs[runIndex].insertRemove=0;
                ++runIndex;
            } while(i<limit);

            if(limit<length) {
                /* there is a separate WS run */
                runs[runIndex].logicalStart=limit;
                runs[runIndex].visualLimit=length-limit;
                /* For the trailing WS run, pBiDi->paraLevel is ok even
                   if contextual multiple paragraphs.                   */
                if(pBiDi->paraLevel<minLevel) {
                    minLevel=pBiDi->paraLevel;
                }
            }

            /* set the object fields */
            pBiDi->runs=runs;
            pBiDi->runCount=runCount;

            reorderLine(pBiDi, minLevel, maxLevel);

            /* now add the direction flags and adjust the visualLimit's to be just that */
            /* this loop will also handle the trailing WS run */
            limit=0;
            for(i=0; i<runCount; ++i) {
                ADD_ODD_BIT_FROM_LEVEL(runs[i].logicalStart, levels[runs[i].logicalStart]);
                limit+=runs[i].visualLimit;
                runs[i].visualLimit=limit;
            }

            /* Set the "odd" bit for the trailing WS run. */
            /* For a RTL paragraph, it will be the *first* run in visual order. */
            /* For the trailing WS run, pBiDi->paraLevel is ok even if
               contextual multiple paragraphs.                          */
            if(runIndex<runCount) {
                int32_t trailingRun = ((pBiDi->paraLevel & 1) != 0)? 0 : runIndex;

                ADD_ODD_BIT_FROM_LEVEL(runs[trailingRun].logicalStart, pBiDi->paraLevel);
            }
        }
    }

    /* handle insert LRM/RLM BEFORE/AFTER run */
    if(pBiDi->insertPoints.size>0) {
        Point *point, *start=pBiDi->insertPoints.points,
                      *limit=start+pBiDi->insertPoints.size;
        int32_t runIndex;
        for(point=start; point<limit; point++) {
            runIndex=getRunFromLogicalIndex(pBiDi, point->pos);
            pBiDi->runs[runIndex].insertRemove|=point->flag;
        }
    }

    /* handle remove BiDi control characters */
    if(pBiDi->controlCount>0) {
        int32_t runIndex;
        const char *start=pBiDi->text, *limit=start+pBiDi->length, *pu;
        for(pu=start; pu<limit; pu++) {
			// FIXME: FIXME: FIXME:
			// FIXME: FIXME: FIXME: Likely to trigger a subtle encoding bug, replace with U8 iteration
            if(IS_BIDI_CONTROL_CHAR(*pu)) {
                runIndex=getRunFromLogicalIndex(pBiDi, (int32_t)(pu-start));
                pBiDi->runs[runIndex].insertRemove--;
            }
        }
    }

    return true;
}

// Static Functions

static void setTrailingWSStart(U8BiDi* pBiDi) {
    /* pBiDi->direction!=UBIDI_MIXED */

    const DirProp *dirProps=pBiDi->dirProps;
    UBiDiLevel *levels=pBiDi->levels;
    int32_t start=pBiDi->length;
    UBiDiLevel paraLevel=pBiDi->paraLevel;

    /* If the line is terminated by a block separator, all preceding WS etc...
       are already set to paragraph level.
       Setting trailingWSStart to pBidi->length will avoid changing the
       level of B chars from 0 to paraLevel in ubidi_getLevels when
       orderParagraphsLTR==true.
     */
    if(dirProps[start-1]==B) {
        pBiDi->trailingWSStart=start;   /* currently == pBiDi->length */
        return;
    }
    /* go backwards across all WS, BN, explicit codes */
    while(start>0 && DIRPROP_FLAG(dirProps[start-1])&MASK_WS) {
        --start;
    }

    /* if the WS run can be merged with the previous run then do so here */
    while(start>0 && levels[start-1]==paraLevel) {
        --start;
    }

    pBiDi->trailingWSStart=start;
}

static void getSingleRun(U8BiDi *pBiDi, UBiDiLevel level) {
    /* simple, single-run case */
    pBiDi->runs=pBiDi->simpleRuns;
    pBiDi->runCount=1;

    /* fill and reorder the single run */
    pBiDi->runs[0].logicalStart=MAKE_INDEX_ODD_PAIR(0, level);
    pBiDi->runs[0].visualLimit=pBiDi->length;
    pBiDi->runs[0].insertRemove=0;
}

static void reorderLine(U8BiDi *pBiDi, UBiDiLevel minLevel, UBiDiLevel maxLevel) {
    Run *runs, tempRun;
    UBiDiLevel *levels;
    int32_t firstRun, endRun, limitRun, runCount;

    /* nothing to do? */
    if(maxLevel<=(minLevel|1)) {
        return;
    }

    /*
     * Reorder only down to the lowest odd level
     * and reorder at an odd minLevel in a separate, simpler loop.
     * See comments above for why minLevel is always incremented.
     */
    ++minLevel;

    runs=pBiDi->runs;
    levels=pBiDi->levels;
    runCount=pBiDi->runCount;

    /* do not include the WS run at paraLevel<=old minLevel except in the simple loop */
    if(pBiDi->trailingWSStart<pBiDi->length) {
        --runCount;
    }

    while(--maxLevel>=minLevel) {
        firstRun=0;

        /* loop for all sequences of runs */
        for(;;) {
            /* look for a sequence of runs that are all at >=maxLevel */
            /* look for the first run of such a sequence */
            while(firstRun<runCount && levels[runs[firstRun].logicalStart]<maxLevel) {
                ++firstRun;
            }
            if(firstRun>=runCount) {
                break;  /* no more such runs */
            }

            /* look for the limit run of such a sequence (the run behind it) */
            for(limitRun=firstRun; ++limitRun<runCount && levels[runs[limitRun].logicalStart]>=maxLevel;) {}

            /* Swap the entire sequence of runs from firstRun to limitRun-1. */
            endRun=limitRun-1;
            while(firstRun<endRun) {
                tempRun = runs[firstRun];
                runs[firstRun]=runs[endRun];
                runs[endRun]=tempRun;
                ++firstRun;
                --endRun;
            }

            if(limitRun==runCount) {
                break;  /* no more such runs */
            } else {
                firstRun=limitRun+1;
            }
        }
    }

    /* now do maxLevel==old minLevel (==odd!), see above */
    if(!(minLevel&1)) {
        firstRun=0;

        /* include the trailing WS run in this complete reordering */
        if(pBiDi->trailingWSStart==pBiDi->length) {
            --runCount;
        }

        /* Swap the entire sequence of all runs. (endRun==runCount) */
        while(firstRun<runCount) {
            tempRun=runs[firstRun];
            runs[firstRun]=runs[runCount];
            runs[runCount]=tempRun;
            ++firstRun;
            --runCount;
        }
    }
}

static int32_t getRunFromLogicalIndex(U8BiDi *pBiDi, int32_t logicalIndex) {
    Run *runs=pBiDi->runs;
    int32_t runCount=pBiDi->runCount, visualStart=0, i, length, logicalStart;

    for(i=0; i<runCount; i++) {
        length=runs[i].visualLimit-visualStart;
        logicalStart=GET_INDEX(runs[i].logicalStart);
        if((logicalIndex>=logicalStart) && (logicalIndex<(logicalStart+length))) {
            return i;
        }
        visualStart+=length;
    }
    /* we should never get here */
	// FIXME: Unreachable
	abort();
}
