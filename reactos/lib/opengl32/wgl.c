/*
 * COPYRIGHT:            See COPYING in the top level directory
 * PROJECT:              ReactOS kernel
 * FILE:                 lib/opengl32/wgl.c
 * PURPOSE:              OpenGL32 lib, rosglXXX functions
 * PROGRAMMER:           Anich Gregor (blight)
 * UPDATE HISTORY:
 *                       Feb 2, 2004: Created
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "teb.h"
#include "opengl32.h"

#ifdef __cplusplus
extern "C" {
#endif//__cplusplus

#ifdef _MSC_VER
#define UNIMPLEMENTED DBGPRINT( "UNIMPLEMENTED" )
#endif//_MSC_VER

/* FUNCTION: Append OpenGL Rendering Context (GLRC) to list
 * ARGUMENTS: [IN] glrc: GLRC to append to list
 */
static
void
WGL_AppendContext( GLRC *glrc )
{
	/* synchronize */
	if (WaitForSingleObject( OPENGL32_processdata.glrc_mutex, INFINITE ) ==
	    WAIT_FAILED)
	{
		DBGPRINT( "Error: WaitForSingleObject() failed (%d)", GetLastError() );
		return; /* FIXME: do we have to expect such an error and handle it? */
	}

	if (OPENGL32_processdata.glrc_list == NULL)
		OPENGL32_processdata.glrc_list = glrc;
	else
	{
		GLRC *p = OPENGL32_processdata.glrc_list;
		while (p->next != NULL)
			p = p->next;
		p->next = glrc;
	}

	/* release mutex */
	if (!ReleaseMutex( OPENGL32_processdata.glrc_mutex ))
		DBGPRINT( "Error: ReleaseMutex() failed (%d)", GetLastError() );
}


/* FUNCTION: Remove OpenGL Rendering Context (GLRC) from list
 * ARGUMENTS: [IN] glrc: GLRC to remove from list
 */
static
void
WGL_RemoveContext( GLRC *glrc )
{
	/* synchronize */
	if (WaitForSingleObject( OPENGL32_processdata.glrc_mutex, INFINITE ) ==
	    WAIT_FAILED)
	{
		DBGPRINT( "Error: WaitForSingleObject() failed (%d)", GetLastError() );
		return; /* FIXME: do we have to expect such an error and handle it? */
	}

	if (glrc == OPENGL32_processdata.glrc_list)
		OPENGL32_processdata.glrc_list = glrc->next;
	else
	{
		GLRC *p = OPENGL32_processdata.glrc_list;
		while (p != NULL)
		{
			if (p->next == glrc)
			{
				p->next = glrc->next;
				return;
			}
			p = p->next;
		}
		DBGPRINT( "Error: GLRC 0x%08x not found in list!", glrc );
	}

	/* release mutex */
	if (!ReleaseMutex( OPENGL32_processdata.glrc_mutex ))
		DBGPRINT( "Error: ReleaseMutex() failed (%d)", GetLastError() );
}

/* FUNCTION: Check wether a GLRC is in the list
 * ARGUMENTS: [IN] glrc: GLRC to remove from list
 */
static
BOOL
WGL_ContainsContext( GLRC *glrc )
{
	GLRC *p;

	/* synchronize */
	if (WaitForSingleObject( OPENGL32_processdata.glrc_mutex, INFINITE ) ==
	    WAIT_FAILED)
	{
		DBGPRINT( "Error: WaitForSingleObject() failed (%d)", GetLastError() );
		return FALSE; /* FIXME: do we have to expect such an error and handle it? */
	}

	p = OPENGL32_processdata.glrc_list;
	while (p != NULL)
	{
		if (p == glrc)
			return TRUE;
		p = p->next;
	}

	/* release mutex */
	if (!ReleaseMutex( OPENGL32_processdata.glrc_mutex ))
		DBGPRINT( "Error: ReleaseMutex() failed (%d)", GetLastError() );

	return FALSE;
}


/* FUNCTION: SetContextCallBack passed to DrvSetContext. Gets called whenever
 *           the current GL context (dispatch table) is to be changed - can
 *           be multiple times for one DrvSetContext call.
 * ARGUMENTS: [IN] table  Function pointer table (first DWORD is number of
 *                        functions)
 * RETURNS: unkown
 */
DWORD
CALLBACK
WGL_SetContextCallBack( const ICDTable *table )
{
/*	UINT i;*/
	TEB *teb;
	PROC *tebTable, *tebDispatchTable;

	teb = NtCurrentTeb();
	tebTable = (PROC *)teb->glTable;
	tebDispatchTable = (PROC *)teb->glDispatchTable;

	DBGPRINT( "Function count: %d\n", table->num_funcs );

	/* save table */
	memcpy( tebTable, table->dispatch_table,
	        sizeof (PROC) * table->num_funcs );
	memset( tebTable + sizeof (PROC) * table->num_funcs, 0,
	        (sizeof (table->dispatch_table) / sizeof (PROC)) -
	        (sizeof (PROC) * table->num_funcs) );

	/* FIXME: pull in software fallbacks -- need mesa */
#if 0 /* unused atm */
	for (i = 0; i < (sizeof (table->dispatch_table) / sizeof (PROC)); i++)
	{
		if (tebTable[i] == NULL)
		{
			/* FIXME: fallback */
			DBGPRINT( "Warning: GL proc #%d is NULL!", i );
		}
	}
#endif

	/* put in empty functions as long as we dont have a fallback */
	#define X(func, ret, typeargs, args, icdidx, tebidx, stack)            \
		if (tebTable[icdidx] == NULL)                                      \
		{                                                                  \
			DBGPRINT( "Warning: GL proc '%s' is NULL", #func );            \
			tebTable[icdidx] = (PROC)glEmptyFunc##stack;                   \
		}
	GLFUNCS_MACRO
	#undef X

	/* fill teb->glDispatchTable for fast calls */
	#define X(func, ret, typeargs, args, icdidx, tebidx, stack)            \
		if (tebidx >= 0)                                                   \
			tebDispatchTable[tebidx] = tebTable[icdidx];
	GLFUNCS_MACRO
	#undef X

	return ERROR_SUCCESS;
}


/* FUNCTION: Attempts to find the best matching pixel format for HDC
 * ARGUMENTS: [IN] pdf  PFD describing what kind of format you want
 * RETURNS: one-based positive format index on success, 0 on failure
 */
#define BUFFERDEPTH_SCORE(want, have) \
	((want == 0) ? (0) : ((want < have) ? (1) : ((want > have) ? (3) : (0))))
int
APIENTRY
rosglChoosePixelFormat( HDC hdc, CONST PIXELFORMATDESCRIPTOR *pfd )
{
	GLDRIVERDATA *icd;
	PIXELFORMATDESCRIPTOR icdPfd;
	int i;
	int best = -1;
	int score, bestScore = 0x7fff; /* used to choose a pfd if no exact match */
	int icdNumFormats;
	const DWORD compareFlags = PFD_DRAW_TO_WINDOW | PFD_DRAW_TO_BITMAP |
	                           PFD_SUPPORT_GDI | PFD_SUPPORT_OPENGL;

	/* load ICD */
	icd = OPENGL32_LoadICDForHDC( hdc );
	if (icd == NULL)
		return 0;

	/* check input */
	if (pfd->nSize != sizeof (PIXELFORMATDESCRIPTOR) || pfd->nVersion != 1)
	{
		SetLastError( 0 ); /* FIXME: use appropriate errorcode */
		return 0;
	}

	/* get number of formats -- FIXME: use 1 or 0 as index? */
	icdNumFormats = icd->DrvDescribePixelFormat( hdc, 1,
	                                  sizeof (PIXELFORMATDESCRIPTOR), NULL );
	if (icdNumFormats == 0)
	{
		DBGPRINT( "DrvDescribePixelFormat failed (%d)", GetLastError() );
		return 0;
	}

	/* try to find best format */
	for (i = 0; i < icdNumFormats; i++)
	{
		if (icd->DrvDescribePixelFormat( hdc, i + 1,
		                         sizeof (PIXELFORMATDESCRIPTOR), &icdPfd ) == 0)
		{
			DBGPRINT( "Warning: DrvDescribePixelFormat failed (%d)",
			          GetLastError() );
			break;
		}

		/* compare flags */
		if ((pfd->dwFlags & compareFlags) != (icdPfd.dwFlags & compareFlags))
			continue;
		if (!(pfd->dwFlags & PFD_DOUBLEBUFFER_DONTCARE) &&
		    ((pfd->dwFlags & PFD_DOUBLEBUFFER) != (icdPfd.dwFlags & PFD_DOUBLEBUFFER)))
			continue;
		if (!(pfd->dwFlags & PFD_STEREO_DONTCARE) &&
		    ((pfd->dwFlags & PFD_STEREO) != (icdPfd.dwFlags & PFD_STEREO)))
			continue;

		/* check other attribs */
		score = 0; /* higher is worse */
		if (pfd->iPixelType != icdPfd.iPixelType)
			score += 5; /* this is really bad i think */
		if (pfd->iLayerType != icdPfd.iLayerType)
			score += 15; /* this is very very bad ;) */

		score += BUFFERDEPTH_SCORE(pfd->cAlphaBits, icdPfd.cAlphaBits);
		score += BUFFERDEPTH_SCORE(pfd->cAccumBits, icdPfd.cAccumBits);
		score += BUFFERDEPTH_SCORE(pfd->cDepthBits, icdPfd.cDepthBits);
		score += BUFFERDEPTH_SCORE(pfd->cStencilBits, icdPfd.cStencilBits);
		score += BUFFERDEPTH_SCORE(pfd->cAuxBuffers, icdPfd.cAuxBuffers);

		/* check score */
		if (score < bestScore)
		{
			bestScore = score;
			best = i + 1;
			if (bestScore == 0)
				break;
		}
	}

	if (best == -1)
	{
		SetLastError( 0 ); /* FIXME: set appropriate error */
		return 0;
	}

	return best;
}


/* FUNCTION: Copy data specified by mask from one GLRC to another.
 * ARGUMENTS: [IN]  src  Source GLRC
 *            [OUT] dst  Destination GLRC
 *            [IN]  mask Bitfield like given to glPushAttrib()
 * RETURN: TRUE on success, FALSE on failure
 */
BOOL
APIENTRY
rosglCopyContext( HGLRC hsrc, HGLRC hdst, UINT mask )
{
	GLRC *src = (GLRC *)hsrc;
	GLRC *dst = (GLRC *)hdst;

	/* check glrcs */
	if (!WGL_ContainsContext( src ))
	{
		DBGPRINT( "Error: src GLRC not found!" );
		return FALSE; /* FIXME: SetLastError() */
	}
	if (!WGL_ContainsContext( dst ))
	{
		DBGPRINT( "Error: dst GLRC not found!" );
		return FALSE; /* FIXME: SetLastError() */
	}

	/* I think this is only possible within one ICD */
	if (src->icd != src->icd)
	{
		DBGPRINT( "Error: src and dst GLRC use different ICDs!" );
		return FALSE;
	}

	/* copy data (call ICD) */
	return src->icd->DrvCopyContext( src->hglrc, dst->hglrc, mask );
}


/* FUNCTION: Create a new GL Rendering Context for the given plane on
 *           the given DC.
 * ARGUMENTS: [IN] hdc   Handle for DC for which to create context
 *            [IN] layer Layer number to bind (draw?) to
 * RETURNS: NULL on failure, new GLRC on success
 */
HGLRC
APIENTRY
rosglCreateLayerContext( HDC hdc, int layer )
{
/*	LONG ret;
	WCHAR driver[256];
	DWORD dw, size;*/

	GLDRIVERDATA *icd = NULL;
	GLRC *glrc;
	HGLRC drvHglrc = NULL;

	if (GetObjectType( hdc ) != OBJ_DC)
	{
		DBGPRINT( "Error: hdc is not a DC handle!" );
		return NULL;
	}

	/* allocate our GLRC */
	glrc = (GLRC*)HeapAlloc( GetProcessHeap(),
	               HEAP_ZERO_MEMORY | HEAP_GENERATE_EXCEPTIONS, sizeof (GLRC) );
	if (glrc == NULL)
		return NULL;

#if 0 /* old code */
	/* try to find an ICD */
	for (dw = 0; drvHglrc == NULL; dw++) /* enumerate values */
	{
		size = sizeof (driver) / sizeof (driver[0]);
		ret = OPENGL32_RegEnumDrivers( dw, driver, &size );
		if (ret != ERROR_SUCCESS)
			break;

		icd = OPENGL32_LoadICD( driver );
		if (icd == NULL) /* try next ICD */
			continue;

		if (icd->DrvCreateLayerContext)
			drvHglrc = icd->DrvCreateLayerContext( hdc, layer );
		if (drvHglrc == NULL)
		{
			if (layer == 0)
				drvHglrc = icd->DrvCreateContext( hdc );
			else
				DBGPRINT( "Warning: CreateLayerContext not supported by ICD!" );
		}
		if (drvHglrc == NULL) /* try next ICD */
		{
			DBGPRINT( "Info: DrvCreateContext (driver = %ws) failed: %d",
			          icd->driver_name, GetLastError() );
			OPENGL32_UnloadICD( icd );
			continue;
		}

		/* the ICD was loaded successfully and we got a HGLRC in drvHglrc */
		break;
	}

	if (drvHglrc == NULL || icd == NULL) /* no ICD was found */
	{
		/* FIXME: fallback to mesa */
		DBGPRINT( "Error: No working ICD found!" );
		HeapFree( GetProcessHeap(), 0, glrc );
		return NULL;
	}
#endif /* unused */

	/* load ICD */
	icd = OPENGL32_LoadICDForHDC( hdc );
	if (icd == NULL)
	{
		DBGPRINT( "Couldn't get ICD by HDC :-(" );
		/* FIXME: fallback? */
		return NULL;
	}

	/* create context */
	if (icd->DrvCreateLayerContext != NULL)
		drvHglrc = icd->DrvCreateLayerContext( hdc, layer );
	if (drvHglrc == NULL)
	{
		if (layer == 0 && icd->DrvCreateContext != NULL)
			drvHglrc = icd->DrvCreateContext( hdc );
		else
			DBGPRINT( "Warning: CreateLayerContext not supported by ICD!" );
	}

	if (drvHglrc == NULL)
	{
		/* FIXME: fallback to mesa? */
		DBGPRINT( "Error: DrvCreate[Layer]Context failed! (%d)", GetLastError() );
		OPENGL32_UnloadICD( icd );
		HeapFree( GetProcessHeap(), 0, glrc );
		return NULL;
	}

	/* we have our GLRC in glrc and the ICD's GLRC in drvHglrc */
	glrc->hglrc = drvHglrc;
	glrc->iFormat = -1; /* what is this used for? */
	glrc->icd = icd;

	/* append glrc to context list */
	WGL_AppendContext( glrc );

	return (HGLRC)glrc;
}


/* FUNCTION: Create a new GL Rendering Context for the given DC.
 * ARGUMENTS: [IN] hdc  Handle for DC for which to create context
 * RETURNS: NULL on failure, new GLRC on success
 */
HGLRC
APIENTRY
rosglCreateContext( HDC hdc )
{
	return rosglCreateLayerContext( hdc, 0 );
}


/* FUNCTION: Delete an OpenGL context
 * ARGUMENTS: [IN] hglrc  Handle to GLRC to delete; must not be a threads RC!
 * RETURNS: TRUE on success, FALSE otherwise
 */
BOOL
APIENTRY
rosglDeleteContext( HGLRC hglrc )
{
	GLRC *glrc = (GLRC *)hglrc;

	/* check if we know about this context */
	if (!WGL_ContainsContext( glrc ))
	{
		DBGPRINT( "Error: hglrc not found!" );
		return FALSE; /* FIXME: SetLastError() */
	}

	/* make sure GLRC is not current for some thread */
	if (glrc->is_current)
	{
		DBGPRINT( "Error: GLRC is current for DC 0x%08x", glrc->hdc );
		return FALSE; /* FIXME: SetLastError() */
	}

	/* release ICD's context */
	if (glrc->hglrc != NULL)
	{
		if (!glrc->icd->DrvDeleteContext( glrc->hglrc ))
		{
			DBGPRINT( "Warning: DrvDeleteContext() failed (%d)", GetLastError() );
			return FALSE;
		}
	}

	/* free resources */
	OPENGL32_UnloadICD( glrc->icd );
	WGL_RemoveContext( glrc );
	HeapFree( GetProcessHeap(), 0, glrc );

	return TRUE;
}


BOOL
APIENTRY
rosglDescribeLayerPlane( HDC hdc, int iPixelFormat, int iLayerPlane,
                                   UINT nBytes, LPLAYERPLANEDESCRIPTOR plpd )
{
	UNIMPLEMENTED;
	return FALSE;
}


int
APIENTRY
rosglDescribePixelFormat( HDC hdc, int iFormat, UINT nBytes,
                                   LPPIXELFORMATDESCRIPTOR pfd )
{
	int ret = 0;
	GLDRIVERDATA *icd = OPENGL32_LoadICDForHDC( hdc );

	if (icd != NULL)
	{
		ret = icd->DrvDescribePixelFormat( hdc, iFormat, nBytes, pfd );
		if (ret == 0)
			DBGPRINT( "Error: DrvDescribePixelFormat failed (%d)", GetLastError() );
	}

	/* FIXME: implement own functionality? */
	return ret;
}


/* FUNCTION: Return the current GLRC
 * RETURNS: Current GLRC (NULL if none was set current)
 */
HGLRC
APIENTRY
rosglGetCurrentContext()
{
	return (HGLRC)(OPENGL32_threaddata->glrc);
}


/* FUNCTION: Return the current DC
 * RETURNS: NULL on failure, current DC otherwise
 */
HDC
APIENTRY
rosglGetCurrentDC()
{
	/* FIXME: is it correct to return NULL when there is no current GLRC or
	   is there another way to find out the wanted HDC? */
	if (OPENGL32_threaddata->glrc == NULL)
		return NULL;
	return (HDC)(OPENGL32_threaddata->glrc->hdc);
}


int
APIENTRY
rosglGetLayerPaletteEntries( HDC hdc, int iLayerPlane, int iStart,
                               int cEntries, COLORREF *pcr )
{
	UNIMPLEMENTED;
	return 0;
}


int
WINAPI
rosglGetPixelFormat( HDC hdc )
{
	UNIMPLEMENTED;
	return 0;
}


/* FUNCTION: Get the address for an OpenGL extension function from the current ICD.
 * ARGUMENTS: [IN] proc:  Name of the function to look for
 * RETURNS: The address of the proc or NULL on failure.
 */
PROC
APIENTRY
rosglGetProcAddress( LPCSTR proc )
{
	if (OPENGL32_threaddata->glrc == NULL)
	{
		DBGPRINT( "Error: No current GLRC!" );
		return NULL;
	}

	if (proc[0] == 'g' && proc[1] == 'l') /* glXXX */
	{
		PROC glXXX = OPENGL32_threaddata->glrc->icd->DrvGetProcAddress( proc );
		if (glXXX)
		{
			DBGPRINT( "Info: Proc \"%s\" loaded from ICD.", proc );
			return glXXX;
		}

		/* FIXME: go through own functions? */
		DBGPRINT( "Warning: Unsupported GL extension: %s", proc );
	}
	if (proc[0] == 'w' && proc[1] == 'g' && proc[2] == 'l') /* wglXXX */
	{
		/* FIXME: support wgl extensions? (there are such IIRC) */
		DBGPRINT( "Warning: Unsupported WGL extension: %s", proc );
	}
	if (proc[0] == 'g' && proc[1] == 'l' && proc[2] == 'u') /* gluXXX */
	{
		/* FIXME: do we support these as well? */
		DBGPRINT( "Warning: GLU extension %s requested, returning NULL", proc );
	}

	return NULL;
}


/* FUNCTION: make the given GLRC the threads current GLRC for hdc
 * ARGUMENTS: [IN] hdc   Handle for a DC to be drawn on
 *            [IN] hglrc Handle for a GLRC to make current
 * RETURNS: TRUE on success, FALSE otherwise
 */
BOOL
APIENTRY
rosglMakeCurrent( HDC hdc, HGLRC hglrc )
{
	GLRC *glrc = (GLRC *)hglrc;

	/* flush current context */
	if (OPENGL32_threaddata->glrc != NULL)
	{
		glFlush();
	}

	/* check hdc */
	if (GetObjectType( hdc ) != OBJ_DC)
	{
		DBGPRINT( "Error: hdc is not a DC handle!" );
		return FALSE;
	}

	/* check if we know about this glrc */
	if (!WGL_ContainsContext( glrc ))
	{
		DBGPRINT( "Error: hglrc not found!" );
		return FALSE; /* FIXME: SetLastError() */
	}

	/* check if it is available */
	if (glrc->is_current) /* used by another thread */
	{
		DBGPRINT( "Error: hglrc is current for thread 0x%08x", glrc->thread_id );
		return FALSE; /* FIXME: SetLastError() */
	}

	/* call the ICD */
	if (glrc->hglrc != NULL)
	{
		if (!glrc->icd->DrvSetContext( hdc, glrc->hglrc,
		                               WGL_SetContextCallBack ))
		{
			DBGPRINT( "Error: DrvSetContext failed (%d)\n", GetLastError() );
			return FALSE;
		}
	}

	/* make it current */
	if (OPENGL32_threaddata->glrc != NULL)
		OPENGL32_threaddata->glrc->is_current = FALSE;
	glrc->is_current = TRUE;
	glrc->thread_id = GetCurrentThreadId();
	glrc->hdc = hdc;
	OPENGL32_threaddata->glrc = glrc;

	return TRUE;
}


BOOL
APIENTRY
rosglRealizeLayerPalette( HDC hdc, int iLayerPlane, BOOL bRealize )
{
	UNIMPLEMENTED;
	return FALSE;
}


int
APIENTRY
rosglSetLayerPaletteEntries( HDC hdc, int iLayerPlane, int iStart,
                               int cEntries, CONST COLORREF *pcr )
{
	UNIMPLEMENTED;
	return 0;
}


BOOL
WINAPI
rosglSetPixelFormat( HDC hdc, int iFormat, CONST PIXELFORMATDESCRIPTOR *pfd )
{
	GLDRIVERDATA *icd;

	icd = OPENGL32_LoadICDForHDC( hdc );
	if (icd == NULL)
		return FALSE;

	if (!icd->DrvSetPixelFormat( hdc, iFormat, pfd ))
	{
		DBGPRINT( "Warning: DrvSetPixelFormat failed (%d)", GetLastError() );
		return FALSE;
	}

	return TRUE;
}


/* FUNCTION: Enable display-list sharing between multiple GLRCs
 * ARGUMENTS: [IN] hglrc1 GLRC number 1
 *            [IN] hglrc2 GLRC number 2
 * RETURNS: TRUR on success, FALSE on failure
 */
BOOL
APIENTRY
rosglShareLists( HGLRC hglrc1, HGLRC hglrc2 )
{
	GLRC *glrc1 = (GLRC *)hglrc1;
	GLRC *glrc2 = (GLRC *)hglrc2;

	/* check glrcs */
	if (!WGL_ContainsContext( glrc1 ))
	{
		DBGPRINT( "Error: hglrc1 not found!" );
		return FALSE; /* FIXME: SetLastError() */
	}
	if (!WGL_ContainsContext( glrc2 ))
	{
		DBGPRINT( "Error: hglrc2 not found!" );
		return FALSE; /* FIXME: SetLastError() */
	}

	/* I think this is only possible within one ICD */
	if (glrc1->icd != glrc2->icd)
	{
		DBGPRINT( "Error: hglrc1 and hglrc2 use different ICDs!" );
		return FALSE;
	}

	/* share lists (call ICD) */
	return glrc1->icd->DrvShareLists( glrc1->hglrc, glrc2->hglrc );
}


/* FUNCTION: Flushes GL and swaps front/back buffer if appropriate
 * ARGUMENTS: [IN] hdc  Handle to device context to swap buffers for
 * RETURNS: TRUE on success, FALSE on failure
 */
BOOL
APIENTRY
rosglSwapBuffers( HDC hdc )
{
#if 0
	/* check if there is a current GLRC */
	if (OPENGL32_threaddata->glrc == NULL)
	{
		DBGPRINT( "Error: No current GL context!" );
		return FALSE;
	}

	/* ask ICD to swap buffers */
	/* FIXME: also ask ICD when we didnt use it to create the context/it couldnt? */
	if (OPENGL32_threaddata->glrc->hglrc != NULL)
	{
		if (!OPENGL32_threaddata->glrc->icd->DrvSwapBuffers( hdc ))
		{
			DBGPRINT( "Error: DrvSwapBuffers failed (%d)", GetLastError() );
			return FALSE;
		}
		return TRUE;
	}
#endif

	GLDRIVERDATA *icd = OPENGL32_LoadICDForHDC( hdc );
	if (icd != NULL)
	{
		if (!icd->DrvSwapBuffers( hdc ))
		{
			DBGPRINT( "Error: DrvSwapBuffers failed (%d)", GetLastError() );
			return FALSE;
		}
		return TRUE;
	}

	/* FIXME: implement own functionality? */
	return FALSE;
}


BOOL
APIENTRY
rosglSwapLayerBuffers( HDC hdc, UINT fuPlanes )
{
	UNIMPLEMENTED;
	return FALSE;
}


BOOL
APIENTRY
rosglUseFontBitmapsA( HDC hdc, DWORD  first, DWORD count, DWORD listBase )
{
	UNIMPLEMENTED;
	return FALSE;
}


BOOL
APIENTRY
rosglUseFontBitmapsW( HDC hdc, DWORD  first, DWORD count, DWORD listBase )
{
	UNIMPLEMENTED;
	return FALSE;
}


BOOL
APIENTRY
rosglUseFontOutlinesA( HDC hdc, DWORD first, DWORD count, DWORD listBase,
                          FLOAT deviation, FLOAT extrusion, int  format,
                          LPGLYPHMETRICSFLOAT  lpgmf )
{
	UNIMPLEMENTED;
	return FALSE;
}


BOOL
APIENTRY
rosglUseFontOutlinesW( HDC hdc, DWORD first, DWORD count, DWORD listBase,
                          FLOAT deviation, FLOAT extrusion, int  format,
                          LPGLYPHMETRICSFLOAT  lpgmf )
{
	UNIMPLEMENTED;
	return FALSE;
}

#ifdef __cplusplus
}; // extern "C"
#endif//__cplusplus

/* EOF */
