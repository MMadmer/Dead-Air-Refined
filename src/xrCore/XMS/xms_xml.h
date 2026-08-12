#pragma once

// XMS XML composition: .xmlp DOM patches applied to a parsed XMLDocument.
// Patch files live in <module>\patch\**\*.xmlp:
//   <xms-patch target="ui\actor_menu.xml">
//     <append into="path:to:node"> ...new child nodes... </append>
//     <set-attr node="path[attr=value]" name="x" value="y"/>
//     <replace node="path"> <single-replacement/> </replace>
//     <remove node="path"/>
//   </xms-patch>
// Node paths use ':' separators; a segment is tag, tag#index or tag[attr=value].

#include "xrCore/xrCore.h"

class XMLDocument;

namespace XMS
{
// Applies every module patch registered for this document. Called from
// XMLDocument::Load after a successful parse. No-op when XMS is inactive.
XRCORE_API void ApplyXmlPatches(XMLDocument& doc, pcstr xml_filename);
}
