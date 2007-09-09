/*
 * Copyright (C) 2005 Casper S. Hornstrup
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include "../../pch.h"

#include "mingw.h"
#include <assert.h>

using std::string;
using std::vector;

ProxyMakefile::ProxyMakefile ( const Project& project )
	: project ( project )
{
}

ProxyMakefile::~ProxyMakefile ()
{
}

bool
ProxyMakefile::GenerateProxyMakefile ( Module& module )
{
	return module.output->directory == OutputDirectory;
}

void
ProxyMakefile::GenerateProxyMakefiles ( bool verbose,
                                        string outputTree )
{
	for ( size_t i = 0; i < project.modules.size (); i++ )
	{
		Module& module = *project.modules[i];
		if ( !module.enabled )
			continue;
		if ( !GenerateProxyMakefile ( module ) )
			continue;
		GenerateProxyMakefileForModule ( module,
		                                 verbose,
		                                 outputTree );
	}
}

string
ProxyMakefile::GeneratePathToParentDirectory ( int numberOfParentDirectories )
{
	string path = "";
	for ( int i = 0; i < numberOfParentDirectories; i++ )
	{
		if ( path != "" )
			path += sSep;
		path += "..";
	}
	return path;
}

string
ProxyMakefile::GetPathToTopDirectory ( Module& module )
{
	int numberOfDirectories = 1;
	string basePath = module.output->relative_path;
	for ( size_t i = 0; i < basePath.length (); i++ )
	{
		if ( basePath[i] == cSep )
			numberOfDirectories++;
	}
	return GeneratePathToParentDirectory ( numberOfDirectories );
}

void
ProxyMakefile::GenerateProxyMakefileForModule ( Module& module,
                                                bool verbose,
                                                string outputTree )
{
	char* buf;
	char* s;

	if ( verbose )
	{
		printf ( "\nGenerating proxy makefile for %s",
		         module.name.c_str () );
	}

	string base;
	string pathToTopDirectory;
	if ( outputTree.length () > 0 )
	{
		base = outputTree + sSep + module.output->relative_path;
		pathToTopDirectory = working_directory;
	}
	else
	{
		base = module.output->relative_path;
		pathToTopDirectory = GetPathToTopDirectory ( module );
	}
	string proxyMakefile = NormalizeFilename ( base + sSep + "GNUmakefile" );
	string defaultTarget = module.name;

	buf = (char*) malloc ( 10*1024 );
	if ( buf == NULL )
		throw OutOfMemoryException ();

	s = buf;
	s = s + sprintf ( s, "# This file is automatically generated.\n" );
	s = s + sprintf ( s, "\n" );
	s = s + sprintf ( s, "TOP = %s\n", pathToTopDirectory.c_str () );
	s = s + sprintf ( s, "DEFAULT = %s\n", defaultTarget.c_str () );
	s = s + sprintf ( s, "include $(TOP)/proxy.mak\n" );

	FileSupportCode::WriteIfChanged ( buf, proxyMakefile, true );

	free ( buf );
}
