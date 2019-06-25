<?xml version="1.0" encoding="ISO-8859-1"?>

<xsl:stylesheet
xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
xmlns="http://www.w3.org/TR/REC-html40"
version="2.0">

	<xsl:output method="html"/>

	<!-- Root template -->
	<xsl:template match="/">
		<html>
			<head>
				<title><xsl:value-of select="/root/display_name" /></title>
				<link rel="stylesheet" type="text/css" href="../css/bpdoc.css" />
			</head>
			<body>
				<div id="content_container">
					<xsl:apply-templates select="/root" />
				</div>
			</body>
		</html>
	</xsl:template>
	
	<xsl:template match="/root">
		<a class="navbar_style">
			<xsl:attribute name="href">../index.html</xsl:attribute>
			<xsl:value-of select="docs_name" />
		</a>
		<a class="navbar_style">&gt;</a>
		<a class="navbar_style"><xsl:value-of select="display_name" /></a>
		<h1 class="title_style"><xsl:value-of select="display_name" /></h1>
		<xsl:apply-templates select="classTree" />
		<xsl:apply-templates select="classDescription" />
		<xsl:apply-templates select="nodes" />
		<xsl:apply-templates select="properties" />
	</xsl:template>

	
		
	<xsl:template match="classDescription">
	<h2 class="title_style">Class Description</h2>
		<xsl:if test="not(classDescriptionData)">
		<span>No Description available</span>
		</xsl:if>
			<span><pre> <xsl:value-of select="classDescriptionData" /> </pre></span>
	<br/>
	</xsl:template>
	<!-- Templates to match specific elements in the input xml -->
	<xsl:template match="nodes">
	
		<h2 class="title_style">Nodes</h2>
	<xsl:if test="not(node)">
		<span>No nodes exposed</span>
	</xsl:if>
	<xsl:if test="node">	
		<table>
			<tbody>
				<xsl:apply-templates select="node">
					<xsl:sort select="shorttitle"/>
				</xsl:apply-templates>
			</tbody>
		</table>	
	</xsl:if>
	</xsl:template>

	<xsl:template match="node">
		<tr>
			<td>
				<a>
					<xsl:attribute name="href">./nodes/<xsl:value-of select="id" />.html</xsl:attribute>
					<xsl:apply-templates select="shorttitle" />	
				</a>
			</td>
		</tr>
	</xsl:template>
	
	
	<xsl:template match="properties">
		<h2 class="title_style">Properties</h2>
		<xsl:if test="property">
		<xsl:call-template name="parameters" />
		</xsl:if>
		<xsl:if test="not(property)">
			<span>No property exposed</span>
		</xsl:if>
	</xsl:template>
	<xsl:template match="property">
		<tr>
			<td>
				<div class="param_name title_style">
					<xsl:apply-templates select="prop_name"/>
				</div>
				<div class="param_type">
					<xsl:apply-templates select="prop_type"/>
				</div>
			</td>
			<td>
				<pre>
					<xsl:apply-templates select="prop_description"/>
				</pre>
			</td>
		</tr>
	</xsl:template>
	
	<xsl:template name="parameters">
		<table>
			<!-- @TODO: This should be done in the css! -->
			<colgroup>
				<col width="25%" />
				<col width="75%" />
			</colgroup>
			<tbody>
				<xsl:apply-templates/>
			</tbody>
		</table>
	</xsl:template>
	
	<xsl:template name="classTree">
		
		<xsl:apply-templates/>
	</xsl:template>
	<xsl:template match="class">
			<span class="fake_tabs">.<xsl:value-of  select="class_indent" disable-output-escaping="yes" /></span>
			<a>
				<xsl:attribute name="href">../<xsl:value-of select="class_name" />/<xsl:value-of select="class_name" />.html</xsl:attribute>
				<xsl:value-of select="class_name" /> 
			</a>
	<br/>
	</xsl:template>
	<xsl:template match="class_UE">
			<span  class="fake_tabs">.<xsl:value-of  select="class_indent" disable-output-escaping="yes" /></span>
			<a>
				<xsl:attribute name="href">https://www.unrealengine.com/bing-search?keyword=<xsl:value-of select="class_name" /></xsl:attribute>
				<xsl:value-of select="class_name" /> 
			</a>
	<br/>
	</xsl:template>

</xsl:stylesheet>
