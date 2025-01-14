// Copyright (C) Remedy Entertainment Plc.

#include "FbxNodeReader.h"

#include "DebugCodes.h"
#include "Helpers.h"
#include "PrecompiledHeader.h"
#include "Tokens.h"

#include <algorithm>
#include <numeric>
#include <utility>

DIAGNOSTIC_PUSH
IGNORE_USD_WARNINGS
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdr/shaderProperty.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdSkel/tokens.h>
#include <pxr/usd/usdSkel/utils.h>

DIAGNOSTIC_POP

#ifdef HOUDINI
#include <hboost/format.hpp>
namespace boost = hboost;
#else
#include <boost/format.hpp>
#endif

#include <fbxsdk.h>

namespace helpers
{
	constexpr double MM_PER_INCH = 25.4;

	template< typename T >
	GfMatrix4d toGfMatrix( const T& m )
	{
		return { m[ 0 ][ 0 ], m[ 0 ][ 1 ], m[ 0 ][ 2 ], m[ 0 ][ 3 ], m[ 1 ][ 0 ], m[ 1 ][ 1 ], m[ 1 ][ 2 ], m[ 1 ][ 3 ],
				 m[ 2 ][ 0 ], m[ 2 ][ 1 ], m[ 2 ][ 2 ], m[ 2 ][ 3 ], m[ 3 ][ 0 ], m[ 3 ][ 1 ], m[ 3 ][ 2 ], m[ 3 ][ 3 ] };
	}

	inline GfVec3f toGfVec( const FbxVector4& src )
	{
		return { static_cast< float >( src[ 0 ] ), static_cast< float >( src[ 1 ] ), static_cast< float >( src[ 2 ] ) };
	}

	inline GfVec3f toGfVec( const FbxColor& src )
	{
		return { static_cast< float >( src.mRed ), static_cast< float >( src.mGreen ), static_cast< float >( src.mBlue ) };
	}

	template< typename T >
	T getAtVertexIndex( const FbxLayerElementTemplate< T >* pLayerElement, int iVertexIndex )
	{
		switch( pLayerElement->GetReferenceMode() )
		{
		case FbxLayerElement::eDirect:
			return pLayerElement->GetDirectArray().GetAt( iVertexIndex );
		case FbxLayerElement::eIndex:
		case FbxLayerElement::eIndexToDirect:
		{
			int id = pLayerElement->GetIndexArray().GetAt( iVertexIndex );
			return pLayerElement->GetDirectArray().GetAt( id );
		}
		}
		return T();
	}

	bool hasVertexColors( const FbxNode* node )
	{
		const auto pMesh = static_cast< const FbxMesh* >( node->GetNodeAttribute() );

		bool res = false;
		for( int i = 0; i < pMesh->GetLayerCount(); ++i )
		{
			const auto layer = pMesh->GetLayer( i );
			res = layer->GetVertexColors() != nullptr;
			if( res )
			{
				break;
			}
		}

		return res;
	}

	const FbxSkin* getSkin( const FbxMesh* mesh )
	{
		for( int deformerId = 0; deformerId < mesh->GetDeformerCount(); ++deformerId )
		{
			const auto deformer = static_cast< const FbxSkin* >( mesh->GetDeformer( deformerId, FbxDeformer::eSkin ) );
			if( deformer )
			{
				return deformer;
			}
		}
		return nullptr;
	}

	// This isn't particularly nice, but I couldn't make it work with less
	// boilerplate code via templates either.
	struct FbxToUsd
	{
		FbxProperty* fbxProperty;

		[[nodiscard]] TfToken getName() const
		{
			return fbxProperty->GetFlag( FbxPropertyFlags::EFlags::eUserDefined ) ? getNameAsUserProperty()
																				  : TfToken( fbxProperty->GetName().Buffer() );
		};

		[[nodiscard]] TfToken getNameAsUserProperty() const
		{
			return TfToken( ( "userProperties:" + fbxProperty->GetName() ).Buffer() );
		};

		[[nodiscard]] SdfValueTypeName getSdfTypeName() const
		{
			switch( fbxProperty->GetPropertyDataType().GetType() )
			{
			case eFbxUChar:
			case eFbxChar:
				return SdfValueTypeNames->UChar;
			case eFbxShort:
				return SdfValueTypeNames->Int;
			case eFbxUShort:
				return SdfValueTypeNames->UInt;
			case eFbxLongLong:
				return SdfValueTypeNames->Int64;
			case eFbxULongLong:
				return SdfValueTypeNames->UInt64;
			case eFbxHalfFloat:
				return SdfValueTypeNames->Half;
			case eFbxBool:
				return SdfValueTypeNames->Bool;
			case eFbxInt:
				return SdfValueTypeNames->Int;
			case eFbxUInt:
				return SdfValueTypeNames->UInt;
			case eFbxDistance:
			case eFbxFloat:
				return SdfValueTypeNames->Float;
			case eFbxDouble:
				return SdfValueTypeNames->Double;
			case eFbxDouble2:
				return SdfValueTypeNames->Double2;
			case eFbxDouble3:
				return SdfValueTypeNames->Double3;
			case eFbxDouble4:
				return SdfValueTypeNames->Double4;
			case eFbxDouble4x4:
				return SdfValueTypeNames->Matrix4d;
			case eFbxTime:
				return SdfValueTypeNames->TimeCode;
			case eFbxBlob:
			case eFbxString:
				return SdfValueTypeNames->Token;
			default:
				return SdfValueTypeNames->Token;
			}
		}

		[[nodiscard]] VtValue getValue() const
		{
			VtValue result;
			switch( fbxProperty->GetPropertyDataType().GetType() )
			{
			case eFbxUChar:
				return VtValue( fbxProperty->Get< FbxUChar >() );
			case eFbxChar:
				// WARNING: Usd only supports unsigned 8bit integers, any value larger
				// than 128 will overflow
				return VtValue( static_cast< uint8_t >( fbxProperty->Get< FbxChar >() ) );
			case eFbxShort:
				// no out of the box 16bit integers in Usd, so we cast to 32bit instead
				return VtValue( static_cast< int32_t >( fbxProperty->Get< FbxShort >() ) );
			case eFbxUShort:
				// no out of the box 16bit integers in Usd, so we cast to 32bit instead
				return VtValue( static_cast< uint32_t >( fbxProperty->Get< FbxUShort >() ) );
			case eFbxLongLong:
				return VtValue( static_cast< int64_t >( fbxProperty->Get< FbxLongLong >() ) );
			case eFbxULongLong:
				return VtValue( static_cast< uint64_t >( fbxProperty->Get< FbxULongLong >() ) );
			case eFbxHalfFloat:
				return VtValue( GfHalf( fbxProperty->Get< FbxHalfFloat >().value() ) );
			case eFbxBool:
				return VtValue( fbxProperty->Get< bool >() );
			case eFbxInt:
				return VtValue( fbxProperty->Get< int32_t >() );
			case eFbxUInt:
				return VtValue( fbxProperty->Get< uint32_t >() );
			case eFbxFloat:
				return VtValue( fbxProperty->Get< float >() );
			case eFbxDouble:
				return VtValue( fbxProperty->Get< double >() );
			case eFbxDouble2:
			{
				const FbxDouble2 d2 = fbxProperty->Get< FbxDouble2 >();
				return VtValue( GfVec2d( d2.mData[ 0 ], d2.mData[ 1 ] ) );
			}
			case eFbxDouble3:
			{
				const FbxDouble3 d3 = fbxProperty->Get< FbxDouble3 >();
				return VtValue( GfVec3d( d3.mData[ 0 ], d3.mData[ 1 ], d3.mData[ 2 ] ) );
			}
			case eFbxDouble4:
			{
				const FbxDouble4 d4 = fbxProperty->Get< FbxDouble4 >();
				return VtValue( GfVec4d( d4.mData[ 0 ], d4.mData[ 1 ], d4.mData[ 2 ], d4.mData[ 3 ] ) );
			}
			case eFbxDouble4x4:
			{
				const FbxMatrix m( fbxProperty->Get< FbxMatrix >() );
				return VtValue( toGfMatrix( m ) );
			}
			case eFbxTime:
				return VtValue( UsdTimeCode( fbxProperty->Get< FbxTime >().GetFrameCountPrecise() ) );
			case eFbxDistance:
				return VtValue( fbxProperty->Get< FbxDistance >().value() );
			case eFbxBlob:
				// Maybe not the most kosher thing on the planet, but eh
				return VtValue( TfToken( static_cast< const char* >( fbxProperty->Get< FbxBlob >().Access() ) ) );
			case eFbxString:
				return VtValue( TfToken( fbxProperty->Get< FbxString >().Buffer() ) );
			default:
				return VtValue( TfToken( "UNKNOWN TYPE" ) );
			}
		}

		[[nodiscard]] VtValue getValue( const std::vector< float >& animChannels ) const
		{
			VtValue result;

			switch( fbxProperty->GetPropertyDataType().GetType() )
			{
			case eFbxBool:
				return VtValue( static_cast< bool >( animChannels[ 0 ] ) );
			case eFbxUChar:
			case eFbxChar:
				return VtValue( static_cast< uint8_t >( animChannels[ 0 ] ) );
			case eFbxShort:
				return VtValue( static_cast< int32_t >( animChannels[ 0 ] ) );
			case eFbxUShort:
				return VtValue( static_cast< uint32_t >( animChannels[ 0 ] ) );
			case eFbxInt:
				return VtValue( static_cast< int32_t >( animChannels[ 0 ] ) );
			case eFbxUInt:
				return VtValue( static_cast< uint32_t >( animChannels[ 0 ] ) );
			case eFbxLongLong:
				return VtValue( static_cast< int64_t >( animChannels[ 0 ] ) );
			case eFbxULongLong:
				return VtValue( static_cast< uint64_t >( animChannels[ 0 ] ) );
			case eFbxHalfFloat:
				return VtValue( GfHalf( animChannels[ 0 ] ) );
			case eFbxFloat:
				return VtValue( animChannels[ 0 ] );
			case eFbxDouble:
				return VtValue( static_cast< double >( animChannels[ 0 ] ) );
			case eFbxDouble2:
				return VtValue( GfVec2d( animChannels[ 0 ], animChannels[ 1 ] ) );
			case eFbxDouble3:
				return VtValue( GfVec3d( animChannels[ 0 ], animChannels[ 1 ], animChannels[ 2 ] ) );
			case eFbxDouble4:
				return VtValue( GfVec4d( animChannels[ 0 ], animChannels[ 1 ], animChannels[ 2 ], animChannels[ 3 ] ) );
			case eFbxDouble4x4:
				return VtValue( GfMatrix4d(
					animChannels[ 0 ],
					animChannels[ 1 ],
					animChannels[ 2 ],
					animChannels[ 3 ],
					animChannels[ 4 ],
					animChannels[ 5 ],
					animChannels[ 6 ],
					animChannels[ 7 ],
					animChannels[ 8 ],
					animChannels[ 9 ],
					animChannels[ 10 ],
					animChannels[ 11 ],
					animChannels[ 12 ],
					animChannels[ 13 ],
					animChannels[ 14 ],
					animChannels[ 15 ] ) );
			default:
				return VtValue( TfToken( "UNKNOWN VALUE" ) );
			}
		}
	};

	std::vector< std::tuple< UsdTimeCode, VtValue > > getPropertyAnimation(
		FbxNode* node,
		std::function< VtValue( FbxNode*, FbxTime ) >& valueAtTimeFn,
		FbxAnimLayer* animLayer,
		FbxTimeSpan& animTimeSpan )
	{
		std::vector< std::tuple< UsdTimeCode, VtValue > > result = {};
		if( animLayer == nullptr )
		{
			return result;
		}

		for( auto frame = animTimeSpan.GetStart().GetFrameCount(); frame <= animTimeSpan.GetStop().GetFrameCount(); ++frame )
		{
			FbxTime currentFrame;
			currentFrame.SetFrame( frame );
			result.push_back( { UsdTimeCode( static_cast< double >( frame ) ), valueAtTimeFn( node, currentFrame ) } );
		}
		return result;
	}

	std::vector< std::tuple< UsdTimeCode, VtValue > > getPropertyAnimation(
		FbxNode* node,
		FbxProperty& fbxProperty,
		FbxAnimLayer* animLayer,
		FbxTimeSpan& animTimeSpan )
	{
		std::vector< std::tuple< UsdTimeCode, VtValue > > result = {};
		if( animLayer == nullptr )
		{
			return result;
		}

		if( !fbxProperty.IsValid() )
		{
			return result;
		}

		const auto curveNode = node->GetAnimationEvaluator()->GetPropertyCurveNode( fbxProperty, animLayer );
		if( curveNode == nullptr )
		{
			return result;
		}

		bool hasAnimCurves = false;
		for( unsigned channelId = 0u; channelId < curveNode->GetChannelsCount(); ++channelId )
		{
			if( curveNode->GetCurve( channelId ) != nullptr )
			{
				hasAnimCurves = true;
				break;
			}
		}

		if( !hasAnimCurves )
		{
			return result;
		}

		const size_t numKeys = animTimeSpan.GetDuration().GetFrameCount() + 1;
		const std::vector< float > defaultChannelsValue( curveNode->GetChannelsCount(), 0.0f );
		std::vector< std::vector< float > > channelValues( numKeys, defaultChannelsValue );
		std::set< UsdTimeCode > timeCodes;

		for( unsigned channelId = 0u; channelId < curveNode->GetChannelsCount(); ++channelId )
		{
			// We are assuming a singular FbxAnimCurve per property, it is however
			// possible to have multiple FbxAnimCurves connected to a singular property
			// If this is deemed necessary, add support for it, otherwise it can be
			// ignored for now see curveNode->GetCurveCount()
			const auto animCurve = curveNode->GetCurve( channelId );
			if( animCurve == nullptr )
			{
				continue;
			}
			// We can't use keyCount, we have to use Evaluate and step through one frame
			// at a time
			size_t index = 0;
			for( auto frame = animTimeSpan.GetStart().GetFrameCount(); frame <= animTimeSpan.GetStop().GetFrameCount(); ++frame )
			{
				timeCodes.insert( UsdTimeCode( static_cast< double >( frame ) ) );
				FbxTime currentFrame;
				currentFrame.SetFrame( frame );
				channelValues[ index ][ channelId ] = animCurve->Evaluate( currentFrame );
				++index;
			}
		}
		FbxToUsd propertyConverter{ &fbxProperty };

		std::transform(
			channelValues.begin(),
			channelValues.end(),
			timeCodes.begin(),
			std::back_inserter( result ),
			[ & ]( const auto& channelValue, const auto& timeCode ) -> std::tuple< UsdTimeCode, VtValue >
			{
				VtValue val = propertyConverter.getValue( channelValue );
				return { timeCode, val };
			} );

		return result;
	}

	std::vector< FbxProperty > getUserProperties( const FbxNode* fbxNode )
	{
		std::vector< FbxProperty > result;
		for( FbxProperty fbxProperty = fbxNode->GetFirstProperty(); fbxProperty.IsValid();
			 fbxProperty = fbxNode->GetNextProperty( fbxProperty ) )
		{
			if( !fbxProperty.GetFlag( FbxPropertyFlags::EFlags::eUserDefined ) )
			{
				continue;
			}
			result.push_back( fbxProperty );
		}
		return result;
	}

	std::vector< FbxProperty > getAnimatedUserProperties( const FbxNode* fbxNode, FbxAnimLayer* animLayer )
	{
		auto res = getUserProperties( fbxNode );
		res.erase(
			std::remove_if(
				res.begin(),
				res.end(),
				[ & ]( FbxProperty& prop ) { return prop.GetCurveNode( animLayer ) == nullptr; } ),
			res.end() );
		return res;
	}

	double toOneTenthOfScene( double value, FbxSystemUnit systemUnits )
	{
		const FbxSystemUnit mmToScene( FbxSystemUnit::mm.GetConversionFactorTo( systemUnits ), 1.0 );
		const double relativeToMM = mmToScene.GetConversionFactorTo( FbxSystemUnit::mm );
		return value * relativeToMM;
	}

	std::pair< TfToken, VtValue > getDisplayGroupMetadata( const TfToken& displayGroupName )
	{
		return { SdfFieldKeys->DisplayGroup, VtValue( displayGroupName.GetString() ) };
	}
} // namespace helpers

namespace converters
{
	GfVec3d translation( const FbxNode* node )
	{
		const auto T = node->LclTranslation.Get();
		return { T[ 0 ], T[ 1 ], T[ 2 ] };
	}

	GfVec3f rotation( const FbxNode* node )
	{
		const auto R = node->LclRotation.Get();
		return { static_cast< float >( R[ 0 ] ), static_cast< float >( R[ 1 ] ), static_cast< float >( R[ 2 ] ) };
	}

	GfVec3f scale( const FbxNode* node )
	{
		const auto S = node->LclScaling.Get();
		return { static_cast< float >( S[ 0 ] ), static_cast< float >( S[ 1 ] ), static_cast< float >( S[ 2 ] ) };
	}

	GfVec3f rotationPivot( const FbxNode* node )
	{
		const auto R = node->RotationPivot.Get();
		return { static_cast< float >( R[ 0 ] ), static_cast< float >( R[ 1 ] ), static_cast< float >( R[ 2 ] ) };
	}

	VtVec3fArray meshPoints( const FbxNode* node )
	{
		VtVec3fArray points;
		// static_cast is used here because at this point we are certain that the node
		// can be cast as an FbxMesh
		const auto pMesh = static_cast< const FbxMesh* >( node->GetNodeAttribute() );
		FbxStatus status;
		FbxVector4* controlPoints = pMesh->GetControlPoints( &status );

		FbxVector4 T = node->GetGeometricTranslation( FbxNode::eSourcePivot );
		FbxVector4 R = node->GetGeometricRotation( FbxNode::eSourcePivot );
		FbxVector4 S = node->GetGeometricScaling( FbxNode::eSourcePivot );

		FbxMatrix geometryToNode;
		geometryToNode.SetTRS( T, R, S );

		std::transform(
			controlPoints,
			controlPoints + pMesh->GetControlPointsCount(),
			std::back_inserter( points ),
			[ & ]( const FbxVector4& v ) { return helpers::toGfVec( geometryToNode.MultNormalize( v ) ); } );
		return points;
	}

	TfToken imageableVisibility( FbxNode* node, FbxTime time )
	{
		const double visibility = node->GetAnimationEvaluator()->GetPropertyValue< FbxDouble >( node->Visibility, time );
		// Visibility is a token in USD. it's either inherited, or "invisible"
		// We essentially check if the level of visibility is close to 0.0, if so, we
		// say it is invisible, otherwise it is inherited For animated visibility, we
		// unfortunately will have to create a custom user attribute, see
		// readImageable for more details
		if( GfIsClose( visibility, 0.0, 1e-6 ) || visibility < 0.0 )
		{
			return UsdGeomTokens->invisible;
		}

		return UsdGeomTokens->inherited;
	}

	VtVec3fArray meshNormals( const FbxNode* node )
	{
		VtVec3fArray normals;
		// static_cast is used here because at this point we are certain that the node
		// can be cast as an FbxMesh
		const auto pMesh = static_cast< const FbxMesh* >( node->GetNodeAttribute() );

		// Find normals
		const FbxLayerElementNormal* perPolygonVertexNormals = nullptr;
		for( int i = 0; i < pMesh->GetLayerCount(); ++i )
		{
			const auto layer = pMesh->GetLayer( i );
			if( const auto normalsElement = layer->GetNormals() )
			{
				if( normalsElement->GetMappingMode() == FbxLayerElement::eByPolygonVertex
					&& normalsElement->GetReferenceMode() != FbxLayerElement::eIndex )
				{
					perPolygonVertexNormals = normalsElement;
				}
			}
		}

		// Parse and convert
		int currentIndex = 0;
		for( int polygonIndex = 0; polygonIndex != pMesh->GetPolygonCount(); ++polygonIndex )
		{
			for( int polygonVertex = 0; polygonVertex != pMesh->GetPolygonSize( polygonIndex ); ++polygonVertex )
			{
				if( perPolygonVertexNormals )
				{
					FbxVector4 normal = helpers::getAtVertexIndex( perPolygonVertexNormals, currentIndex );
					normals.push_back( helpers::toGfVec( normal ) );
					++currentIndex;
				}
				else
				{
					FbxVector4 normal;
					if( pMesh->GetPolygonVertexNormal( polygonIndex, polygonVertex, normal ) )
					{
						normals.push_back( helpers::toGfVec( normal ) );
					}
				}
			}
		}

		return normals;
	}

	VtVec3fArray meshTangents( const FbxNode* node )
	{
		VtVec3fArray tangents;
		// static_cast is used here because at this point we are certain that the node
		// can be cast as an FbxMesh
		const auto pMesh = static_cast< const FbxMesh* >( node->GetNodeAttribute() );

		// Find tangents
		const FbxLayerElementTangent* perPolygonVertexTangents = nullptr;
		for( int i = 0; i < pMesh->GetLayerCount(); ++i )
		{
			const auto layer = pMesh->GetLayer( i );
			if( const auto tangentsElement = layer->GetTangents() )
			{
				if( tangentsElement->GetMappingMode() == FbxLayerElement::eByPolygonVertex
					&& tangentsElement->GetReferenceMode() != FbxLayerElement::eIndex )
				{
					perPolygonVertexTangents = tangentsElement;
				}
			}
		}

		if( !perPolygonVertexTangents )
		{
			return tangents;
		}

		// Parse and convert
		int currentIndex = 0;
		for( int polygonIndex = 0; polygonIndex != pMesh->GetPolygonCount(); ++polygonIndex )
		{
			for( int polygonVertex = 0; polygonVertex != pMesh->GetPolygonSize( polygonIndex ); ++polygonVertex )
			{
				FbxVector4 normal = helpers::getAtVertexIndex( perPolygonVertexTangents, currentIndex );
				tangents.push_back( helpers::toGfVec( normal ) );
				++currentIndex;
			}
		}

		return tangents;
	}

	VtIntArray meshFaceVertexIndices( const FbxNode* node )
	{
		VtIntArray faceVertexIndices;
		const auto pMesh = static_cast< const FbxMesh* >( node->GetNodeAttribute() );

		auto vertexIndices = pMesh->GetPolygonVertices();

		for( int polygonIndex = 0; polygonIndex != pMesh->GetPolygonCount(); ++polygonIndex )
		{
			const int start = pMesh->GetPolygonVertexIndex( polygonIndex );
			for( int polygonVertex = 0; polygonVertex != pMesh->GetPolygonSize( polygonIndex ); ++polygonVertex )
			{
				faceVertexIndices.push_back( vertexIndices[ start + polygonVertex ] );
			}
		}
		return faceVertexIndices;
	}

	VtIntArray meshFaceVertexCounts( const FbxNode* node )
	{
		VtIntArray faceVertexCounts;
		const auto pMesh = static_cast< const FbxMesh* >( node->GetNodeAttribute() );
		for( int polygonIndex = 0; polygonIndex != pMesh->GetPolygonCount(); ++polygonIndex )
		{
			faceVertexCounts.push_back( pMesh->GetPolygonSize( polygonIndex ) );
		}
		return faceVertexCounts;
	}

	VtVec3fArray meshVertexColors( const FbxNode* node )
	{
		VtVec3fArray colors;
		const auto pMesh = static_cast< const FbxMesh* >( node->GetNodeAttribute() );

		const FbxLayerElementVertexColor* perPolygonVertexColors = nullptr;
		for( int i = 0; i < pMesh->GetLayerCount(); ++i )
		{
			const auto layer = pMesh->GetLayer( i );
			if( const auto vertexColorsElement = layer->GetVertexColors() )
			{
				if( vertexColorsElement->GetMappingMode() != FbxLayerElement::eByControlPoint
					&& vertexColorsElement->GetReferenceMode() != FbxLayerElement::eIndex )
				{
					continue;
				}
				perPolygonVertexColors = vertexColorsElement;
			}
		}

		if( !perPolygonVertexColors )
		{
			return colors;
		}

		// Parse and convert
		for( int i = 0; i != pMesh->GetControlPointsCount(); ++i )
		{
			FbxColor color = perPolygonVertexColors->GetDirectArray().GetAt( i );
			colors.push_back( GfVec3f(
				static_cast< float >( color.mRed ),
				static_cast< float >( color.mGreen ),
				static_cast< float >( color.mBlue ) ) );
		}

		return colors;
	}

	VtVec2fArray meshTexCoords( const FbxNode* node, int layerIndex )
	{
		VtVec2fArray texCoords;

		const auto pMesh = static_cast< const FbxMesh* >( node->GetNodeAttribute() );
		const auto uvLayerElement = pMesh->GetLayer( layerIndex )->GetUVs();

		// Parse and convert
		int currentIndex = 0;
		for( int polygonIndex = 0; polygonIndex != pMesh->GetPolygonCount(); ++polygonIndex )
		{
			for( int polygonVertex = 0; polygonVertex != pMesh->GetPolygonSize( polygonIndex ); ++polygonVertex )
			{
				FbxVector2 uv = helpers::getAtVertexIndex( uvLayerElement, currentIndex );
				texCoords.push_back( GfVec2f( static_cast< float >( uv[ 0 ] ), static_cast< float >( uv[ 1 ] ) ) );
				++currentIndex;
			}
		}

		return texCoords;
	}

	double cameraApertureHeight( const FbxCamera* camera )
	{
		return helpers::toOneTenthOfScene(
			camera->FilmHeight * camera->FilmSqueezeRatio * helpers::MM_PER_INCH,
			camera->GetScene()->GetGlobalSettings().GetSystemUnit() );
	}

	double cameraApertureWidth( const FbxCamera* camera )
	{
		return helpers::toOneTenthOfScene(
			camera->FilmWidth * camera->FilmSqueezeRatio * helpers::MM_PER_INCH,
			camera->GetScene()->GetGlobalSettings().GetSystemUnit() );
	}

	TfToken cameraProjectionMode( const FbxCamera* camera )
	{
		switch( camera->ProjectionType )
		{
		case( FbxCamera::EProjectionType::ePerspective ):
			return UsdGeomTokens->perspective;
		case( FbxCamera::EProjectionType::eOrthogonal ):
			return UsdGeomTokens->orthographic;
		}
		return UsdGeomTokens->perspective;
	}

	GfVec2f cameraClippingRange( const FbxCamera* camera )
	{
		return { static_cast< float >( camera->NearPlane ), static_cast< float >( camera->FarPlane ) };
	}

	double cameraFocalLength( FbxCamera* camera, FbxTime t = FbxTime(), bool scale = false )
	{
		const double focalLength
			= camera->GetNode()->GetAnimationEvaluator()->GetPropertyValue< FbxDouble >( camera->FocalLength, t );

		return scale ? helpers::toOneTenthOfScene( focalLength, camera->GetScene()->GetGlobalSettings().GetSystemUnit() )
					 : focalLength;
	}

	float cameraFieldOfView( FbxCamera* camera, FbxTime t = FbxTime() )
	{
		return static_cast< float >(
			camera->GetNode()->GetAnimationEvaluator()->GetPropertyValue< FbxDouble >( camera->FieldOfView, t ) );
	}

	TfToken skeletonToTokenPath( const FbxSkeleton* skeleton, TfToken rootJointName )
	{
		// Note: If perf is an issue with this, resort to some kind of caching
		// mechanism.
		TfToken jointName( skeleton->GetNode()->GetName() );
		if( jointName == rootJointName )
		{
			return jointName;
		}

		FbxNode* parent = skeleton->GetNode()->GetParent();
		SdfPath jointPath = SdfPath( parent->GetName() ).AppendChild( jointName );
		while( parent->GetName() != rootJointName )
		{
			parent = parent->GetParent();
			jointPath = SdfPath( parent->GetName() ).AppendPath( jointPath );
		}
		return jointPath.GetAsToken();
	}

	// Skeleton hierarchies in UsdSkel are expressed as an array of TfTokeens in a
	// order dependent `joints` property Each entry in this `joints` attribute must
	// have the full path the root joint This function essentially builds those full
	// paths from a list of FbxSkeletons
	VtTokenArray skeletonHierarchyToTokenList( const std::vector< const FbxSkeleton* >& skeletonHierarchy )
	{
		const TfToken rootJointName( skeletonHierarchy[ 0 ]->GetNode()->GetName() );
		VtTokenArray jointHierarchyAttribute;
		std::transform(
			skeletonHierarchy.cbegin(),
			skeletonHierarchy.cend(),
			std::back_inserter( jointHierarchyAttribute ),
			[ & ]( const FbxSkeleton* skeleton ) -> TfToken { return skeletonToTokenPath( skeleton, rootJointName ); } );
		return jointHierarchyAttribute;
	}

	enum class Space
	{
		Local,
		World
	};

	VtMatrix4dArray skeletonHierarchyToMatrices(
		const std::vector< const FbxSkeleton* >& skeletonHierarchy,
		double scaleFactor,
		Space space )
	{
		VtMatrix4dArray output;
		const auto animEvaluator = skeletonHierarchy[ 0 ]->GetScene()->GetAnimationEvaluator();
		output.reserve( skeletonHierarchy.size() );
		std::transform(
			skeletonHierarchy.cbegin(),
			skeletonHierarchy.cend(),
			std::back_inserter( output ),
			[ & ]( const FbxSkeleton* skeleton ) -> GfMatrix4d
			{
				auto matrix = space == Space::Local ? animEvaluator->GetNodeLocalTransform( skeleton->GetNode() )
													: animEvaluator->GetNodeGlobalTransform( skeleton->GetNode() );
				// We have to force the scale component of the resulting matrix to
				// be 1.0 If there's any LclScaling present on a limbnode, that gets
				// applied to the rotation, but not the translation for some ungodly
				// reason
				matrix.SetS( { 1.0, 1.0, 1.0 } );
				// Due to the above, we also scale the translation from the originally
				// authored coords into the exported file unit scale so it matches what
				// we output in USD as metersPerUnit
				matrix.SetTOnly( matrix.GetT() * scaleFactor );
				return helpers::toGfMatrix( matrix );
			} );
		return output;
	}

	struct BindingData
	{
		VtTokenArray names;
		VtIntArray perVertexInfluences;
		VtFloatArray perVertexWeights;
		int influencesPerVertex;
		SdfPath pathToSkeleton;
	};

	BindingData getBindingData( const FbxSkin* skin, const FbxMesh* mesh )
	{
		if( skin->GetClusterCount() == 0 )
		{
			return { VtTokenArray(), VtIntArray(), VtFloatArray(), 0, SdfPath::EmptyPath() };
		}

		VtTokenArray jointsUsed;
		size_t elementSize = 0;
		jointsUsed.reserve( skin->GetClusterCount() );
		std::vector< std::vector< std::tuple< int, double > > > perVertexIndicesAndWeights(
			mesh->GetControlPointsCount(),
			std::vector< std::tuple< int, double > >() );

		auto* rootBone = skin->GetCluster( 0 )->GetLink();
		while( rootBone )
		{
			auto* newParent = rootBone->GetParent();
			if( !newParent->GetNodeAttribute()
				|| newParent->GetNodeAttribute()->GetAttributeType() != FbxNodeAttribute::eSkeleton )
			{
				break;
			}
			rootBone = newParent;
		}
		const TfToken rootBoneName( rootBone->GetName() );

		for( int clusterId = 0; clusterId < skin->GetClusterCount(); clusterId++ )
		{
			const FbxCluster* cluster = skin->GetCluster( clusterId );
			const auto link = cluster->GetLink();
			if( !link )
			{
				continue;
			}

			const int* controlPointIndices = cluster->GetControlPointIndices();
			const double* controlPointWeights = cluster->GetControlPointWeights();
			for( int controlPointId = 0; controlPointId < cluster->GetControlPointIndicesCount(); ++controlPointId )
			{
				const size_t influenceIndex = jointsUsed.size();

				perVertexIndicesAndWeights[ controlPointIndices[ controlPointId ] ].push_back(
					{ influenceIndex, controlPointWeights[ controlPointId ] } );
				const size_t numInfluences = perVertexIndicesAndWeights[ controlPointIndices[ controlPointId ] ].size();
				elementSize = std::max( numInfluences, elementSize );
			}

			TfToken skeletonPath(
				skeletonToTokenPath( static_cast< const FbxSkeleton* >( link->GetNodeAttribute() ), rootBoneName ) );
			jointsUsed.push_back( skeletonPath );
		}

		// split the aggregated per-vertex vector into two individual vectors for
		// indices and weights. all entries must be of the same element size, add
		// missing empty weight values where necessary.
		VtIntArray jointIndices;
		jointIndices.reserve( mesh->GetControlPointsCount() * elementSize );
		VtFloatArray jointWeights;
		jointWeights.reserve( mesh->GetControlPointsCount() * elementSize );
		const auto missingValue = std::make_tuple< int, double >( 0, 0.0 );
		for( const auto& vertexInfluencesAndWeights : perVertexIndicesAndWeights )
		{
			const size_t lastIndex = elementSize - ( elementSize - vertexInfluencesAndWeights.size() );
			for( size_t i = 0; i < elementSize; ++i )
			{
				auto [ influenceIndex, weight ] = i < lastIndex ? vertexInfluencesAndWeights[ i ] : missingValue;
				jointIndices.push_back( influenceIndex );
				jointWeights.push_back( static_cast< float >( weight ) );
			}
		}

		const int influencesPerComponents = static_cast< int >( elementSize );

		UsdSkelNormalizeWeights( jointWeights, influencesPerComponents );
		UsdSkelSortInfluences( jointIndices, jointWeights, influencesPerComponents );

		SdfPath pathToSkeleton( "/ROOT" );
		{
			const FbxNode* parent = rootBone->GetParent();
			SdfPath jointPath = SdfPath( TfToken( rootBone->GetName() ) );
			while( parent && parent != parent->GetScene()->GetRootNode() )
			{
				jointPath = SdfPath( parent->GetName() ).AppendPath( jointPath );
				parent = parent->GetParent();
			}
			pathToSkeleton = pathToSkeleton.AppendPath( jointPath );
		}
		return { jointsUsed, jointIndices, jointWeights, influencesPerComponents, pathToSkeleton };
	}
} // namespace converters

namespace
{
	void readMetadata( remedy::FbxNodeReaderContext& context )
	{
		context.GetOrAddPrim().metadata[ SdfFieldKeys->Active ] = VtValue( true );
		context.GetOrAddPrim().metadata[ SdfFieldKeys->Hidden ] = VtValue( false );
		std::string comment = std::string( "Converted from \"" ) + context.GetNode()->GetName() + ( "\"" );
		context.GetOrAddPrim().metadata[ SdfFieldKeys->Comment ] = VtValue( comment.c_str() );
	}

	void readUnknown( remedy::FbxNodeReaderContext& context )
	{
		TF_DEBUG( USDFBX_FBX_READERS ).Msg( "UsdFbx::FbxReaders - readUnknown for \"%s\"\n", context.GetNode()->GetName() );
		auto& prim = context.GetOrAddPrim();
		prim.typeName = UsdFbxPrimTypeNames->Scope;
	}

	void readImageable( remedy::FbxNodeReaderContext& context )
	{
		TF_DEBUG( USDFBX_FBX_READERS ).Msg( "UsdFbx::FbxReaders - readImageable for \"%s\"\n", context.GetNode()->GetName() );
		context.CreateProperty(
			UsdGeomTokens->visibility,
			SdfValueTypeNames->Token,
			VtValue( converters::imageableVisibility( context.GetNode(), FbxTime() ) ),
			[]( FbxNode* node, FbxTime time ) { return VtValue( converters::imageableVisibility( node, time ) ); },
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->imageable ) } );

		context.CreateUniformProperty(
			UsdGeomTokens->purpose,
			SdfValueTypeNames->Token,
			VtValue( TfToken( "default" ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->imageable ) } );

		context.CreateProperty(
			TfToken( "generated:" + UsdGeomTokens->visibility.GetString() ),
			SdfValueTypeNames->Double,
			VtValue( static_cast< double >( context.GetNode()->Visibility ) ),
			&context.GetNode()->Visibility,
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->generated ),
			  { SdfFieldKeys->Custom, VtValue( true ) } } );
	}

	void readUserProperties( remedy::FbxNodeReaderContext& context )
	{
		TF_DEBUG( USDFBX_FBX_READERS )
			.Msg( "UsdFbx::FbxReaders - readUserProperties for \"%s\"\n", context.GetNode()->GetName() );
		for( FbxProperty fbxProperty : helpers::getUserProperties( context.GetNode() ) )
		{
			helpers::FbxToUsd propertyConverter{ &fbxProperty };
			auto valueType = propertyConverter.getSdfTypeName();
			auto defaultValue = propertyConverter.getValue();

			const auto cleanedName = cleanName( fbxProperty.GetName().Buffer(), " _", remedy::FbxNameFixer() );
			TfToken propertyName( "userProperties:" + cleanedName );
			context.CreateProperty(
				propertyName,
				valueType,
				std::move( defaultValue ),
				&fbxProperty,
				{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->user ),
				  { SdfFieldKeys->Custom, VtValue( true ) } } );
		}
	}

	void readCamera( remedy::FbxNodeReaderContext& context )
	{
		TF_DEBUG( USDFBX_FBX_READERS ).Msg( "UsdFbx::FbxReaders - readCamera for \"%s\"\n", context.GetNode()->GetName() );
		context.GetOrAddPrim().typeName = UsdFbxPrimTypeNames->Camera;
		FbxNode* fbxNode = context.GetNode();
		if( !fbxNode->GetNodeAttribute() || fbxNode->GetNodeAttributeCount() == 0
			|| fbxNode->GetNodeAttribute()->GetAttributeType() != FbxNodeAttribute::eCamera )
		{
			return;
		}

		auto* camera = fbxNode->GetCamera();

		context.CreateProperty(
			UsdGeomTokens->focalLength,
			SdfValueTypeNames->Float,
			VtValue( static_cast< float >( converters::cameraFocalLength( camera, FbxTime(), true ) ) ),
			[]( FbxNode* node, FbxTime t ) { return VtValue( converters::cameraFocalLength( node->GetCamera(), t, true ) ); },
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->camera ) } );

		context.CreateProperty(
			UsdGeomTokens->focusDistance,
			SdfValueTypeNames->Float,
			VtValue( static_cast< float >( camera->FocusDistance ) ),
			&camera->FocusDistance,
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->camera ) } );

		// Both horizontal and vertical aperture is stored as inches in FBX, because
		// of course it is
		context.CreateProperty(
			UsdGeomTokens->horizontalAperture,
			SdfValueTypeNames->Float,
			VtValue( static_cast< float >( converters::cameraApertureWidth( camera ) ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->camera ) } );

		context.CreateProperty(
			UsdGeomTokens->verticalAperture,
			SdfValueTypeNames->Float,
			VtValue( static_cast< float >( converters::cameraApertureHeight( camera ) ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->camera ) } );

		context.CreateProperty(
			UsdGeomTokens->projection,
			SdfValueTypeNames->Token,
			VtValue( converters::cameraProjectionMode( camera ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->camera ) } );

		// Fbx does not seem to define an fstop, so we force it always to zero
		if( camera->UseDepthOfField )
		{
			context.CreateProperty(
				UsdGeomTokens->fStop,
				SdfValueTypeNames->Float,
				VtValue( 0.0f ),
				{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->camera ) } );
		}

		context.CreateProperty(
			UsdGeomTokens->clippingRange,
			SdfValueTypeNames->Float2,
			VtValue( converters::cameraClippingRange( camera ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->camera ) } );

		context.CreateProperty(
			TfToken( "generated:fov" ),
			SdfValueTypeNames->Float,
			VtValue( converters::cameraFieldOfView( camera ) ),
			[]( FbxNode* node, FbxTime t ) { return VtValue( converters::cameraFieldOfView( node->GetCamera(), t ) ); },
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->generated ),
			  { SdfFieldKeys->Custom, VtValue( true ) } } );
	}

	void readMesh( remedy::FbxNodeReaderContext& context )
	{
		TF_DEBUG( USDFBX_FBX_READERS ).Msg( "UsdFbx::FbxReaders - readMesh for \"%s\"\n", context.GetNode()->GetName() );
		context.GetOrAddPrim().typeName = UsdFbxPrimTypeNames->Mesh;

		const FbxNode* fbxNode = context.GetNode();
		if( !fbxNode->GetNodeAttribute() || fbxNode->GetNodeAttributeCount() == 0
			|| fbxNode->GetNodeAttribute()->GetAttributeType() != FbxNodeAttribute::eMesh )
		{
			return;
		}

		// Varying/Interpolated properties
		context.CreateProperty(
			UsdGeomTokens->points,
			SdfValueTypeNames->Point3fArray,
			VtValue( converters::meshPoints( context.GetNode() ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->geometry ) } );

		// TODO - Post 1.0: potentially use primvars:normals/tangents instead.
		// primvars:normals/tangents takes precendence over
		// UsdGeomPointBased::normals/tangents
		context.CreateProperty(
			UsdGeomTokens->normals,
			SdfValueTypeNames->Normal3fArray,
			VtValue( converters::meshNormals( context.GetNode() ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->geometry ),
			  { UsdGeomTokens->interpolation, VtValue( UsdGeomTokens->faceVarying ) } } );

		context.CreateProperty(
			UsdGeomTokens->tangents,
			SdfValueTypeNames->Normal3fArray,
			VtValue( converters::meshTangents( context.GetNode() ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->geometry ),
			  { UsdGeomTokens->interpolation, VtValue( UsdGeomTokens->faceVarying ) } } );

		if( helpers::hasVertexColors( fbxNode ) )
		{
			context.CreateProperty(
				UsdGeomTokens->primvarsDisplayColor,
				SdfValueTypeNames->Color3f,
				VtValue( converters::meshVertexColors( context.GetNode() ) ),
				nullptr,
				// TODO - Post 1.0: Add fbx property for color animation
				{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->geometry ),
				  { UsdGeomTokens->interpolation, VtValue( UsdGeomTokens->vertex ) } } );
		}

		context.CreateProperty(
			UsdGeomTokens->faceVertexCounts,
			SdfValueTypeNames->IntArray,
			VtValue( converters::meshFaceVertexCounts( context.GetNode() ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->geometry ) } );

		context.CreateProperty(
			UsdGeomTokens->faceVertexIndices,
			SdfValueTypeNames->IntArray,
			VtValue( converters::meshFaceVertexIndices( context.GetNode() ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->geometry ) } );

		if( const auto* skin = helpers::getSkin( static_cast< const FbxMesh* >( fbxNode->GetNodeAttribute() ) ) )
		{
			context.GetOrAddPrim().metadata.emplace(
				UsdTokens->apiSchemas,
				VtValue( SdfTokenListOp::Create( { TfToken( "SkelBindingAPI" ) } ) ) );

			const auto& [ joints, jointIndices, jointWeights, elementSize, skeletonPath ]
				= converters::getBindingData( skin, static_cast< const FbxMesh* >( fbxNode->GetNodeAttribute() ) );

			if( joints.empty() )
			{
				TF_WARN(
					"A skin for \"%s\" has been defined, but no joints could be "
					"extracted!",
					fbxNode->GetName() );
			}
			else
			{
				auto matrix = fbxNode->GetScene()->GetAnimationEvaluator()->GetNodeGlobalTransform( context.GetNode() );
				matrix.SetS( { 1.0, 1.0, 1.0 } );
				const GfMatrix4d geomBindTransform( helpers::toGfMatrix( matrix ) );

				// Specify which joints are actually used

				context.CreateUniformProperty(
					UsdSkelTokens->skelJoints,
					SdfValueTypeNames->TokenArray,
					VtValue( joints ),
					{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skeleton ) } );

				// Joint indices
				context.CreateProperty(
					UsdSkelTokens->primvarsSkelJointIndices,
					SdfValueTypeNames->IntArray,
					VtValue( jointIndices ),
					{
						{ UsdGeomTokens->interpolation, VtValue( UsdGeomTokens->vertex ) },
						{ UsdGeomTokens->elementSize, VtValue( elementSize ) },
						helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skeleton ),
					} );

				// Joint weights
				context.CreateProperty(
					UsdSkelTokens->primvarsSkelJointWeights,
					SdfValueTypeNames->FloatArray,
					VtValue( jointWeights ),
					{ { UsdGeomTokens->interpolation, VtValue( UsdGeomTokens->vertex ) },
					  { UsdGeomTokens->elementSize, VtValue( elementSize ) },
					  helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skeleton ) } );

				// bind transform
				context.CreateProperty(
					UsdSkelTokens->primvarsSkelGeomBindTransform,
					SdfValueTypeNames->Matrix4d,
					VtValue( geomBindTransform ),
					{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skeleton ) } );

				// Relationship to the skeleton
				// Add skeleton relationship property
				context.CreateRelationship(
					UsdSkelTokens->skelSkeleton,
					skeletonPath,
					{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skeleton ) } );
			}
		}

		// Special case for UVs as we may end up with or or more properties per UV
		// channel Scoped because do not need mesh after this anymore
		{
			const auto mesh = static_cast< const FbxMesh* >( fbxNode->GetNodeAttribute() );
			const int layerCount = mesh->GetLayerCount();
			for( int i = 0; i != layerCount; ++i )
			{
				const auto layer = mesh->GetLayer( i );

				const auto layerElement = layer->GetUVs();
				if( !layerElement || layerElement->GetMappingMode() != FbxLayerElement::eByPolygonVertex
					|| layerElement->GetReferenceMode() == FbxLayerElement::eIndex )
				{
					continue;
				}
				std::string suffix = layerCount > 1 ? ( boost::format( "_%1%" ) % layerElement->GetName() ).str() : "";
				context.CreateProperty(
					TfToken( ( boost::format( "primvars:st%1%" ) % suffix ).str().c_str() ),
					SdfValueTypeNames->TexCoord2fArray,
					VtValue( converters::meshTexCoords( context.GetNode(), i ) ),
					nullptr,
					{ { UsdGeomTokens->interpolation, VtValue( UsdGeomTokens->faceVarying ) },
					  helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->geometry ) } );
			}
		}

		// This property does not matter when dealing with pre-defined normals
		// It is essentially a hint to the renderer that if normals need to be
		// calculated on the fly, which orientation to take We set it now to
		// rightHanded as that is the default, it's ignored if normals are authored on
		// the layer (at least in most Hydra renderers)
		context.CreateUniformProperty(
			UsdGeomTokens->orientation,
			SdfValueTypeNames->Token,
			VtValue( UsdGeomTokens->rightHanded ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->geometry ) } );

		context.CreateUniformProperty(
			UsdGeomTokens->subdivisionScheme,
			SdfValueTypeNames->Token,
			VtValue( UsdGeomTokens->none ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->geometry ) } );
	}

	void readSkeletonAnimation( remedy::FbxNodeReaderContext& context )
	{
		TF_DEBUG( USDFBX_FBX_READERS ).Msg( "UsdFbx::FbxReaders - readSkeletonAnim for \"%s\"\n", context.GetNode()->GetName() );
		if( context.GetAnimLayer() == nullptr )
		{
			return;
		}

		const FbxNode* fbxNode = context.GetNode();
		const FbxNode* parent = fbxNode->GetParent();
		const auto pSkeleton = static_cast< const FbxSkeleton* >( fbxNode->GetNodeAttribute() );

		auto isSkeleton = []( const FbxNode* node )
		{
			return node->GetNodeAttribute() && node->GetNodeAttributeCount() > 0
				   && node->GetNodeAttribute()->GetAttributeType() == FbxNodeAttribute::eSkeleton;
		};

		// Skip any child skeletons, they are handled when the first joint is
		// encountered
		if( parent && isSkeleton( parent ) )
		{
			return;
		}

		const TfToken skelAnimationPrimName( std::string( "Animation" ) + fbxNode->GetName() );

		const auto parentPath = context.GetPath().GetParentPath();
		const auto skelAnimPrimPath = parentPath.AppendChild( skelAnimationPrimName );

		if( auto parentPrim = context.GetPrimAtPath( parentPath ) )
		{
			parentPrim.value()->children.push_back( skelAnimationPrimName );
		}
		else
		{
			TF_WARN( "readSkeletonAnimation: Unable to find a parent at path @%s@", parentPath.GetAsString().c_str() );
		}

		auto& skeletonAnimPrim = context.AddPrim( skelAnimPrimPath );
		skeletonAnimPrim.typeName = UsdFbxPrimTypeNames->SkelAnimation;

		std::vector skeletonHierarchy{ pSkeleton };
		// Using a fully qualified type so we can call the lambda recursively
		std::function< void( const FbxSkeleton* ) > collectSkeletonHierarchy = [ & ]( const FbxSkeleton* skeleton )
		{
			for( size_t i = 0, n = skeleton->GetNode()->GetChildCount(); i < n; ++i )
			{
				const FbxNode* child = skeleton->GetNode()->GetChild( static_cast< int >( i ) );
				if( !isSkeleton( child ) )
				{
					TF_WARN(
						"\"%s\" is not an FbxSkeleton node, but is part of a "
						"skeleton hierarchy! It and its children will be ignored",
						child->GetName() );
					continue;
				}

				const FbxSkeleton* childSkeleton = static_cast< const FbxSkeleton* >( child->GetNodeAttribute() );
				skeletonHierarchy.push_back( childSkeleton );
				collectSkeletonHierarchy( childSkeleton );
			}
		};
		collectSkeletonHierarchy( pSkeleton );
		VtTokenArray skeletonTokens = converters::skeletonHierarchyToTokenList( skeletonHierarchy );

		struct Property
		{
			TfToken name;
			SdfValueTypeName typeName;
			std::vector< VtValue > values = {};
			VtTokenArray ownerPaths = {};
			std::map< UsdTimeCode, std::vector< VtValue > > timeSamples = {};
		};

		FbxTime fbxSampleTime = context.GetAnimTimeSpan().GetStart();
		const FbxTime fbxFrameIncrement( FbxTime::GetOneFrameValue( fbxNode->GetScene()->GetGlobalSettings().GetTimeMode() ) );
		auto evaluator = fbxNode->GetScene()->GetAnimationEvaluator();
		const auto numFrames = static_cast< uint64_t >( context.GetAnimTimeSpan().GetDuration().GetFrameCount() );
		std::vector< std::tuple< UsdTimeCode, VtValue > > translations;
		std::vector< std::tuple< UsdTimeCode, VtValue > > rotations;
		std::vector< std::tuple< UsdTimeCode, VtValue > > scales;
		std::map< TfToken, Property > propertiesMap;

		// Parse user properties differently than per-frame skeleton transforms.
		size_t idx = 0;
		for( auto* skeleton : skeletonHierarchy )
		{
			auto fbxProps = helpers::getAnimatedUserProperties( skeleton->GetNode(), context.GetAnimLayer() );
			if( skeleton->GetNode()->Visibility.GetCurveNode( context.GetAnimLayer() ) )
			{
				fbxProps.push_back( skeleton->GetNode()->Visibility );
			}

			TfToken skeletonPath = skeletonTokens[ idx++ ];
			for( auto& fbxProp : fbxProps )
			{
				helpers::FbxToUsd converter{ &fbxProp };
				auto propertiesMapIt = propertiesMap.find( converter.getNameAsUserProperty() );
				if( propertiesMapIt == propertiesMap.end() )
				{
					propertiesMapIt = std::get< 0 >( propertiesMap.insert(
						{ converter.getNameAsUserProperty(),
						  { converter.getNameAsUserProperty(), converter.getSdfTypeName().GetArrayType() } } ) );
				}

				auto& prop = propertiesMapIt->second;
				auto timeAndValue = helpers::getPropertyAnimation(
					skeleton->GetNode(),
					fbxProp,
					context.GetAnimLayer(),
					context.GetAnimTimeSpan() );
				for( auto& [ time, value ] : timeAndValue )
				{
					auto it = prop.timeSamples.find( time );
					if( it == prop.timeSamples.end() )
					{
						it = std::get< 0 >( prop.timeSamples.insert( { time, {} } ) );
					}
					it->second.push_back( value );
				}
				prop.values.push_back( converter.getValue() );
				prop.ownerPaths.push_back( skeletonPath );
			}
		}

		for( uint64_t frame = 0; frame <= numFrames; ++frame )
		{
			VtVec3fArray skeletonTranslations;
			VtQuatfArray skeletonRotations;
			VtVec3hArray skeletonScales;
			UsdTimeCode t( fbxSampleTime.GetFrameCountPrecise() );

			for( const auto* skeleton : skeletonHierarchy )
			{
				const GfMatrix4d local
					= helpers::toGfMatrix( evaluator->GetNodeLocalTransform( skeleton->GetNode(), fbxSampleTime ) );
				skeletonTranslations.push_back( GfVec3f( local.ExtractTranslation() ) );
				skeletonRotations.push_back( GfQuatf( local.ExtractRotationQuat() ) );
				skeletonScales.push_back( GfVec3h( 1.0f, 1.0f, 1.0f ) );
			}

			translations.push_back( { t, VtValue( skeletonTranslations ) } );
			rotations.push_back( { t, VtValue( skeletonRotations ) } );
			scales.push_back( { t, VtValue( skeletonScales ) } );

			fbxSampleTime += fbxFrameIncrement;
		}

		// Figure out if there is actual animation in the individual channels,
		// fetching the matrices every frame doesn't really mean much if all the
		// values are the same
		const bool hasUniqueScales = !std::all_of(
			scales.begin() + 1,
			scales.end(),
			[ & ]( const auto tup ) { return std::get< 1 >( tup ) == std::get< 1 >( scales[ 0 ] ); } );

		context.CreateUniformProperty(
			skelAnimPrimPath.AppendProperty( UsdSkelTokens->joints ),
			SdfValueTypeNames->TokenArray,
			VtValue( skeletonTokens ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skelanimation ) } );

		auto& translationsProp = context.CreateProperty(
			skelAnimPrimPath.AppendProperty( UsdSkelTokens->translations ),
			SdfValueTypeNames->Float3Array,
			VtValue( std::get< 1 >( translations[ 0 ] ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skelanimation ) } );
		translationsProp.timeSamples = translations;

		auto& rotationsProp = context.CreateProperty(
			skelAnimPrimPath.AppendProperty( UsdSkelTokens->rotations ),
			SdfValueTypeNames->QuatfArray,
			VtValue( std::get< 1 >( rotations[ 0 ] ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skelanimation ) } );
		rotationsProp.timeSamples = rotations;

		auto& scalesProp = context.CreateProperty(
			skelAnimPrimPath.AppendProperty( UsdSkelTokens->scales ),
			SdfValueTypeNames->Half3Array,
			VtValue( std::get< 1 >( scales[ 0 ] ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skelanimation ) } );

		if( hasUniqueScales )
		{
			scalesProp.timeSamples = scales;
		}

		// Scalar property animations
		for( auto& [ propName, prop ] : propertiesMap )
		{
			auto& usdProp = context.CreateProperty(
				skelAnimPrimPath.AppendProperty( propName ),
				prop.typeName,
				VtValue( std::move( prop.values ) ),
				{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->user ),
				  { SdfFieldKeys->Custom, VtValue( true ) } } );
			usdProp.timeSamples
				= std::vector< std::tuple< UsdTimeCode, VtValue > >( prop.timeSamples.begin(), prop.timeSamples.end() );

			// add special property to indicate this custom property's owner (joint
			// path)
			context.CreateUniformProperty(
				skelAnimPrimPath.AppendProperty( TfToken( prop.name.GetString() + ":owner" ) ),
				SdfValueTypeNames->TokenArray,
				VtValue( prop.ownerPaths ),
				{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->user ),
				  { SdfFieldKeys->Custom, VtValue( true ) } } );
		}

		// Relationship to the skeleton
		SdfPath pathToSkeleton( "/ROOT" );
		pathToSkeleton = pathToSkeleton.AppendChild( TfToken( fbxNode->GetName() ) );
		context.CreateRelationship(
			pathToSkeleton.AppendProperty( UsdSkelTokens->skelAnimationSource ),
			skelAnimPrimPath,
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skelanimation ) } );
	}

	void readSkeleton( remedy::FbxNodeReaderContext& context )
	{
		TF_DEBUG( USDFBX_FBX_READERS ).Msg( "UsdFbx::FbxReaders - readSkeleton for \"%s\"\n", context.GetNode()->GetName() );
		const FbxNode* fbxNode = context.GetNode();
		const FbxNode* parent = fbxNode->GetParent();

		auto isSkeleton = []( const FbxNode* node )
		{
			return node->GetNodeAttribute() && node->GetNodeAttributeCount() > 0
				   && node->GetNodeAttribute()->GetAttributeType() == FbxNodeAttribute::eSkeleton;
		};

		if( !isSkeleton( fbxNode ) )
		{
			return;
		}

		const auto pSkeleton = static_cast< const FbxSkeleton* >( fbxNode->GetNodeAttribute() );

		const TfToken skeletonPrimName( fbxNode->GetName() );

		// Skip any child skeletons, they are handled when the first joint is
		// encountered
		if( parent && isSkeleton( parent ) )
		{
			return;
		}

		const auto parentPath = context.GetPath().GetParentPath();
		const auto skeletonPrimPath = parentPath.AppendChild( skeletonPrimName );

		if( auto parentPrim = context.GetPrimAtPath( parentPath ) )
		{
			parentPrim.value()->children.push_back( skeletonPrimName );
		}
		else
		{
			TF_WARN( "readSkeleton: Unable to find a parent at path @%s@", parentPath.GetAsString().c_str() );
		}

		auto& skeletonPrim = context.AddPrim( skeletonPrimPath );
		skeletonPrim.typeName = UsdFbxPrimTypeNames->Skeleton;

		std::vector skeletonHierarchy{ pSkeleton };
		// Using a fully qualified type so we can call the lambda recursively
		std::function< void( const FbxSkeleton* ) > collectSkeletonHierarchy = [ & ]( const FbxSkeleton* skeleton )
		{
			for( size_t i = 0, n = skeleton->GetNode()->GetChildCount(); i < n; ++i )
			{
				const FbxNode* child = skeleton->GetNode()->GetChild( static_cast< int >( i ) );
				if( !isSkeleton( child ) )
				{
					TF_WARN(
						"\"%s\" is not an FbxSkeleton node, but is part of a "
						"skeleton hierarchy! It and its children will be ignored",
						child->GetName() );
					continue;
				}

				const FbxSkeleton* childSkeleton = static_cast< const FbxSkeleton* >( child->GetNodeAttribute() );
				skeletonHierarchy.push_back( childSkeleton );
				collectSkeletonHierarchy( childSkeleton );
			}
		};
		collectSkeletonHierarchy( pSkeleton );

		const auto scaleFactor = fbxNode->GetScene()->GetGlobalSettings().GetSystemUnit().GetConversionFactorFrom(
			fbxNode->GetScene()->GetGlobalSettings().GetOriginalSystemUnit() );

		context.CreateUniformProperty(
			UsdSkelTokens->joints,
			SdfValueTypeNames->TokenArray,
			VtValue( converters::skeletonHierarchyToTokenList( skeletonHierarchy ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skeleton ) } );
		context.CreateUniformProperty(
			UsdSkelTokens->restTransforms,
			SdfValueTypeNames->Matrix4dArray,
			VtValue( converters::skeletonHierarchyToMatrices( skeletonHierarchy, scaleFactor, converters::Space::Local ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skeleton ) } );
		context.CreateUniformProperty(
			UsdSkelTokens->bindTransforms,
			SdfValueTypeNames->Matrix4dArray,
			VtValue( converters::skeletonHierarchyToMatrices( skeletonHierarchy, 1.0, converters::Space::World ) ),
			{ helpers::getDisplayGroupMetadata( UsdFbxDisplayGroupTokens->skeleton ) } );
	}

	void readTransform( remedy::FbxNodeReaderContext& context )
	{
		TF_DEBUG( USDFBX_FBX_READERS ).Msg( "UsdFbx::FbxReaders - readTransform for \"%s\"\n", context.GetNode()->GetName() );
		context.GetOrAddPrim().typeName = UsdFbxPrimTypeNames->Xform;
		// Unfortunately, this has to be done to be compliant with UsdXformCommonAPI,
		// Otherwise one could write out additional xformOps for pre and post rotation
		// But doing anything with xformcommonAPI when there's a pre and/or post xform
		// op in the list will not fly
		context.GetNode()->ResetPivotSetAndConvertAnimation();

		const TfToken translate = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeTranslate );
		const TfToken pivot = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeTranslate, UsdGeomTokens->pivot );
		const TfToken pivotInv = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeTranslate, UsdGeomTokens->pivot, true );
		const TfToken scale = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeScale );

		TfToken rotate;
		switch( context.GetNode()->RotationOrder.Get() )
		{
		case FbxEuler::eOrderXYZ:
			rotate = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeRotateXYZ );
			break;
		case FbxEuler::eOrderXZY:
			rotate = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeRotateXZY );
			break;
		case FbxEuler::eOrderYXZ:
			rotate = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeRotateYXZ );
			break;
		case FbxEuler::eOrderYZX:
			rotate = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeRotateYZX );
			break;
		case FbxEuler::eOrderZXY:
			rotate = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeRotateZXY );
			break;
		case FbxEuler::eOrderZYX:
			rotate = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeRotateZYX );
			break;
		case FbxEuler::eOrderSphericXYZ:
		{
			TF_WARN( "SphericXYZ is not supported! A standard XYZ rotation order will "
					 "be used instead, this could result in unwanted behavior!" );
			rotate = UsdGeomXformOp::GetOpName( UsdGeomXformOp::TypeRotateXYZ );
			break;
		}
		}
		// Scale and rotate pivots are collapsed into a singular translate/inv
		// translate pivot op Usually the order is [translate, translatePivot, ... ,
		// !invert!translatePivot] where ... are any of the rotation/scale/etc... ops
		context.CreateProperty(
			translate,
			SdfValueTypeNames->Double3,
			VtValue( converters::translation( context.GetNode() ) ),
			&context.GetNode()->LclTranslation );

		context.CreateProperty(
			pivot,
			SdfValueTypeNames->Double3,
			VtValue( converters::rotationPivot( context.GetNode() ) ),
			&context.GetNode()->RotationPivot );

		context.CreateProperty(
			rotate,
			SdfValueTypeNames->Float3,
			VtValue( converters::rotation( context.GetNode() ) ),
			&context.GetNode()->LclRotation );

		context.CreateProperty(
			scale,
			SdfValueTypeNames->Float3,
			VtValue( converters::scale( context.GetNode() ) ),
			&context.GetNode()->LclScaling );

		context.CreateUniformProperty(
			UsdGeomTokens->xformOpOrder,
			SdfValueTypeNames->TokenArray,
			VtValue( VtTokenArray( { translate, pivot, rotate, scale, pivotInv } ) ) );
	}
} // namespace

remedy::FbxNodeReaders::FbxNodeReaders()
{
	FbxNodeReaderFnContainer unknownReader;
	unknownReader.AddReader( readUnknown );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eUnknown, std::move( unknownReader ) );

	FbxNodeReaderFnContainer nullReader;
	nullReader.AddReader( readTransform ).AddReader( readImageable ).AddReader( readUserProperties );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eNull, std::move( nullReader ) );

	FbxNodeReaderFnContainer meshReader;
	meshReader.AddReader( readTransform ).AddReader( readImageable ).AddReader( readMesh ).AddReader( readUserProperties );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eMesh, meshReader );

	FbxNodeReaderFnContainer skeletonReader;
	// Note on user properties: The skeleton setup is pretty whack compared to
	// Fbx, so user properties are aggregated and written in
	// readSkeleton/Animation.
	skeletonReader.AddReader( readSkeleton ).AddReader( readSkeletonAnimation ).AddReader( readImageable );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eSkeleton, skeletonReader );

	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eNurbs, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::ePatch, FbxNodeReaderFnContainer() );

	FbxNodeReaderFnContainer cameraReader;
	cameraReader.AddReader( readTransform ).AddReader( readImageable ).AddReader( readCamera ).AddReader( readUserProperties );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eCamera, cameraReader );

	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eCameraStereo, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eCameraSwitcher, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eLight, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eOpticalReference, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eOpticalMarker, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eNurbsCurve, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eTrimNurbsSurface, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eBoundary, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eNurbsSurface, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eShape, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eLODGroup, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eSubDiv, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eCachedEffect, FbxNodeReaderFnContainer() );
	m_nodeTypeReaderMap.emplace( FbxNodeAttribute::eLine, FbxNodeReaderFnContainer() );
}

remedy::FbxNodeReaderContext::FbxNodeReaderContext(
	UsdFbxDataReader& dataReader,
	FbxNode* node,
	SdfPath path,
	FbxAnimLayer* animLayer,
	FbxTimeSpan animTimeSpan,
	double scaleFactor )
	: m_dataReader( dataReader )
	, m_fbxNode( node )
	, m_usdPath( std::move( path ) )
	, m_fbxAnimLayer( animLayer )
	, m_fbxTimeSpan( std::move( animTimeSpan ) )
	, m_scaleFactor( scaleFactor )
{
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::createPropertyAtPath( const TfToken& name ) const
{
	return createPropertyAtPath( GetPath().AppendProperty( name ) );
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::createPropertyAtPath( const SdfPath& path ) const
{
	return *m_dataReader.AddProperty( path ).value();
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateUniformProperty(
	const SdfPath& propertyPath,
	const SdfValueTypeName& typeName,
	VtValue&& defaultValue,
	MetadataMap&& metadata )
{
	return CreateProperty(
		propertyPath,
		typeName,
		std::move( defaultValue ),
		nullptr,
		std::move( metadata ),
		SdfVariabilityUniform );
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateUniformProperty(
	const TfToken& propertyName,
	const SdfValueTypeName& typeName,
	VtValue&& defaultValue,
	MetadataMap&& metadata )
{
	return CreateProperty(
		propertyName,
		typeName,
		std::move( defaultValue ),
		nullptr,
		std::move( metadata ),
		SdfVariabilityUniform );
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateProperty(
	const SdfPath& propertyPath,
	const SdfValueTypeName& typeName,
	VtValue&& defaultValue,
	MetadataMap&& metadata,
	SdfVariability variability )
{
	return CreateProperty( propertyPath, typeName, std::move( defaultValue ), nullptr, std::move( metadata ), variability );
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateProperty(
	const TfToken& propertyName,
	const SdfValueTypeName& typeName,
	VtValue&& defaultValue,
	MetadataMap&& metadata,
	SdfVariability variability )
{
	return CreateProperty(
		GetPath().AppendProperty( propertyName ),
		typeName,
		std::move( defaultValue ),
		nullptr,
		std::move( metadata ),
		variability );
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateProperty(
	const TfToken& propertyName,
	const SdfValueTypeName& typeName,
	VtValue&& defaultValue,
	FbxProperty* fbxProperty,
	MetadataMap&& metadata,
	SdfVariability variability )
{
	return CreateProperty(
		GetPath().AppendProperty( propertyName ),
		typeName,
		std::move( defaultValue ),
		fbxProperty,
		std::move( metadata ),
		variability );
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateProperty(
	const SdfPath& propertyPath,
	const SdfValueTypeName& typeName,
	VtValue&& defaultValue,
	FbxProperty* fbxProperty,
	MetadataMap&& metadata,
	SdfVariability variability )
{
	auto& prop = createPropertyAtPath( propertyPath );
	prop.metadata = std::move( metadata );
	prop.typeName = typeName;
	prop.variability = variability;
	if( fbxProperty != nullptr )
	{
		prop.timeSamples = helpers::getPropertyAnimation( GetNode(), *fbxProperty, GetAnimLayer(), GetAnimTimeSpan() );
	}
	prop.value = std::move( defaultValue );
	return prop;
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateProperty(
	const TfToken& propertyName,
	const SdfValueTypeName& typeName,
	VtValue&& defaultValue,
	std::function< VtValue( FbxNode*, FbxTime ) >&& valueAtTimeFn,
	MetadataMap&& metadata,
	SdfVariability variability )
{
	return CreateProperty(
		GetPath().AppendProperty( propertyName ),
		typeName,
		std::move( defaultValue ),
		std::move( valueAtTimeFn ),
		std::move( metadata ),
		variability );
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateProperty(
	const SdfPath& propertyPath,
	const SdfValueTypeName& typeName,
	VtValue&& defaultValue,
	std::function< VtValue( FbxNode*, FbxTime ) >&& valueAtTimeFn,
	MetadataMap&& metadata,
	SdfVariability variability )
{
	auto& prop = createPropertyAtPath( propertyPath );
	prop.metadata = std::move( metadata );
	prop.typeName = typeName;
	prop.variability = variability;
	prop.timeSamples = helpers::getPropertyAnimation( GetNode(), valueAtTimeFn, GetAnimLayer(), GetAnimTimeSpan() );
	prop.value = std::move( defaultValue );
	return prop;
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateRelationship(
	const TfToken& fromProperty,
	const SdfPath& to,
	MetadataMap&& metadata )
{
	return CreateRelationship( GetPath().AppendProperty( fromProperty ), to, std::move( metadata ) );
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateRelationship(
	const SdfPath& from,
	const SdfPath& to,
	MetadataMap&& metadata )
{
	// SdfValueTypeNames and the defaultValue are just fill in values, they do not
	// matter in the end
	auto& prop
		= CreateProperty( from, SdfValueTypeNames->Token, VtValue(), nullptr, std::move( metadata ), SdfVariabilityUniform );
	prop.targetPaths.push_back( to );
	return prop;
}

remedy::FbxNodeReaderContext::Property& remedy::FbxNodeReaderContext::CreateConnection(
	const SdfPath& sourcePath,
	const TfToken& sourceAttribute,
	const SdfPath& targetPath,
	const TfToken& targetAttribute,
	const SdfValueTypeName& targetTypeName,
	MetadataMap&& metadata )
{
	const SdfPath relationshipPath
		= sourcePath.AppendProperty( sourceAttribute ).AppendTarget( targetPath ).AppendRelationalAttribute( targetAttribute );

	const auto valueType = SdfSchema::GetInstance().FindType( "void" );

	const SdfPath sourcePropertyPath = sourcePath.AppendProperty( sourceAttribute );
	const SdfPath targetPropertyPath = targetPath.AppendProperty( targetAttribute );

	auto& sourceProperty = CreateProperty( sourcePropertyPath, valueType, VtValue(), nullptr, MetadataMap( metadata ) );
	// copying metadata here, it's moved later
	sourceProperty.metadata[ SdfFieldKeys->ConnectionPaths ] = VtValue( SdfPathListOp::Create( { targetPropertyPath } ) );

	CreateProperty( sourcePropertyPath, targetTypeName, VtValue(), nullptr, std::move( metadata ) );
	return sourceProperty;
}
