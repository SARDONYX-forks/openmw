﻿#include "worldspacedata.hpp"

#include <components/bullethelpers/aabb.hpp>
#include <components/bullethelpers/heightfield.hpp>
#include <components/debug/debuglog.hpp>
#include <components/detournavigator/gettilespositions.hpp>
#include <components/detournavigator/objectid.hpp>
#include <components/detournavigator/recastmesh.hpp>
#include <components/detournavigator/tilecachedrecastmeshmanager.hpp>
#include <components/esm/cellref.hpp>
#include <components/esm/esmreader.hpp>
#include <components/esm/loadcell.hpp>
#include <components/esm/loadland.hpp>
#include <components/esmloader/esmdata.hpp>
#include <components/esmloader/lessbyid.hpp>
#include <components/esmloader/record.hpp>
#include <components/misc/coordinateconverter.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/stringops.hpp>
#include <components/resource/bulletshapemanager.hpp>
#include <components/settings/settings.hpp>
#include <components/vfs/manager.hpp>

#include <LinearMath/btVector3.h>

#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace NavMeshTool
{
    namespace
    {
        using DetourNavigator::CollisionShape;
        using DetourNavigator::HeightfieldPlane;
        using DetourNavigator::HeightfieldShape;
        using DetourNavigator::HeightfieldSurface;
        using DetourNavigator::ObjectId;
        using DetourNavigator::ObjectTransform;

        struct CellRef
        {
            ESM::RecNameInts mType;
            ESM::RefNum mRefNum;
            std::string mRefId;
            float mScale;
            ESM::Position mPos;

            CellRef(ESM::RecNameInts type, ESM::RefNum refNum, std::string&& refId, float scale, const ESM::Position& pos)
                : mType(type), mRefNum(refNum), mRefId(std::move(refId)), mScale(scale), mPos(pos) {}
        };

        ESM::RecNameInts getType(const EsmLoader::EsmData& esmData, std::string_view refId)
        {
            const auto it = std::lower_bound(esmData.mRefIdTypes.begin(), esmData.mRefIdTypes.end(),
                                             refId, EsmLoader::LessById {});
            if (it == esmData.mRefIdTypes.end() || it->mId != refId)
                return {};
            return it->mType;
        }

        std::vector<CellRef> loadCellRefs(const ESM::Cell& cell, const EsmLoader::EsmData& esmData,
            std::vector<ESM::ESMReader>& readers)
        {
            std::vector<EsmLoader::Record<CellRef>> cellRefs;

            for (std::size_t i = 0; i < cell.mContextList.size(); i++)
            {
                ESM::ESMReader& reader = readers[static_cast<std::size_t>(cell.mContextList[i].index)];
                cell.restore(reader, static_cast<int>(i));
                ESM::CellRef cellRef;
                bool deleted = false;
                while (ESM::Cell::getNextRef(reader, cellRef, deleted))
                {
                    Misc::StringUtils::lowerCaseInPlace(cellRef.mRefID);
                    const ESM::RecNameInts type = getType(esmData, cellRef.mRefID);
                    if (type == ESM::RecNameInts {})
                        continue;
                    cellRefs.emplace_back(deleted, type, cellRef.mRefNum, std::move(cellRef.mRefID),
                                        cellRef.mScale, cellRef.mPos);
                }
            }

            Log(Debug::Debug) << "Loaded " << cellRefs.size() << " cell refs";

            const auto getKey = [] (const EsmLoader::Record<CellRef>& v) -> const ESM::RefNum& { return v.mValue.mRefNum; };
            std::vector<CellRef> result = prepareRecords(cellRefs, getKey);

            Log(Debug::Debug) << "Prepared " << result.size() << " unique cell refs";

            return result;
        }

        template <class F>
        void forEachObject(const ESM::Cell& cell, const EsmLoader::EsmData& esmData, const VFS::Manager& vfs,
            Resource::BulletShapeManager& bulletShapeManager, std::vector<ESM::ESMReader>& readers,
            F&& f)
        {
            std::vector<CellRef> cellRefs = loadCellRefs(cell, esmData, readers);

            Log(Debug::Debug) << "Prepared " << cellRefs.size() << " unique cell refs";

            for (CellRef& cellRef : cellRefs)
            {
                std::string model(getModel(esmData, cellRef.mRefId, cellRef.mType));
                if (model.empty())
                    continue;

                if (cellRef.mType != ESM::REC_STAT)
                    model = Misc::ResourceHelpers::correctActorModelPath(model, &vfs);

                osg::ref_ptr<const Resource::BulletShape> shape = [&]
                {
                    try
                    {
                        return bulletShapeManager.getShape("meshes/" + model);
                    }
                    catch (const std::exception& e)
                    {
                        Log(Debug::Warning) << "Failed to load cell ref \"" << cellRef.mRefId << "\" model \"" << model << "\": " << e.what();
                        return osg::ref_ptr<const Resource::BulletShape>();
                    }
                } ();

                if (shape == nullptr || shape->mCollisionShape == nullptr)
                    continue;

                osg::ref_ptr<Resource::BulletShapeInstance> shapeInstance(new Resource::BulletShapeInstance(std::move(shape)));

                switch (cellRef.mType)
                {
                    case ESM::REC_ACTI:
                    case ESM::REC_CONT:
                    case ESM::REC_DOOR:
                    case ESM::REC_STAT:
                        f(BulletObject(std::move(shapeInstance), cellRef.mPos, cellRef.mScale));
                        break;
                    default:
                        break;
                }
            }
        }

        struct GetXY
        {
            osg::Vec2i operator()(const ESM::Land& value) const { return osg::Vec2i(value.mX, value.mY); }
        };

        struct LessByXY
        {
            bool operator ()(const ESM::Land& lhs, const ESM::Land& rhs) const
            {
                return GetXY {}(lhs) < GetXY {}(rhs);
            }

            bool operator ()(const ESM::Land& lhs, const osg::Vec2i& rhs) const
            {
                return GetXY {}(lhs) < rhs;
            }

            bool operator ()(const osg::Vec2i& lhs, const ESM::Land& rhs) const
            {
                return lhs < GetXY {}(rhs);
            }
        };

        btAABB getAabb(const osg::Vec2i& cellPosition, btScalar minHeight, btScalar maxHeight)
        {
            btAABB aabb;
            aabb.m_min = btVector3(
                static_cast<btScalar>(cellPosition.x() * ESM::Land::REAL_SIZE),
                static_cast<btScalar>(cellPosition.y() * ESM::Land::REAL_SIZE),
                minHeight
            );
            aabb.m_min = btVector3(
                static_cast<btScalar>((cellPosition.x() + 1) * ESM::Land::REAL_SIZE),
                static_cast<btScalar>((cellPosition.y() + 1) * ESM::Land::REAL_SIZE),
                maxHeight
            );
            return aabb;
        }

        void mergeOrAssign(const btAABB& aabb, btAABB& target, bool& initialized)
        {
            if (initialized)
                return target.merge(aabb);

            target.m_min = aabb.m_min;
            target.m_max = aabb.m_max;
            initialized = true;
        }

        std::tuple<HeightfieldShape, float, float> makeHeightfieldShape(const std::optional<ESM::Land>& land,
            const osg::Vec2i& cellPosition, std::vector<std::vector<float>>& heightfields,
            std::vector<std::unique_ptr<ESM::Land::LandData>>& landDatas)
        {
            if (!land.has_value() || osg::Vec2i(land->mX, land->mY) != cellPosition
                    || (land->mDataTypes & ESM::Land::DATA_VHGT) == 0)
                return {HeightfieldPlane {ESM::Land::DEFAULT_HEIGHT}, ESM::Land::DEFAULT_HEIGHT, ESM::Land::DEFAULT_HEIGHT};

            ESM::Land::LandData& landData = *landDatas.emplace_back(std::make_unique<ESM::Land::LandData>());
            land->loadData(ESM::Land::DATA_VHGT, &landData);
            heightfields.emplace_back(std::vector(std::begin(landData.mHeights), std::end(landData.mHeights)));
            HeightfieldSurface surface;
            surface.mHeights = heightfields.back().data();
            surface.mMinHeight = landData.mMinHeight;
            surface.mMaxHeight = landData.mMaxHeight;
            surface.mSize = static_cast<std::size_t>(ESM::Land::LAND_SIZE);
            return {surface, landData.mMinHeight, landData.mMaxHeight};
        }
    }

    WorldspaceNavMeshInput::WorldspaceNavMeshInput(std::string worldspace, const DetourNavigator::RecastSettings& settings)
        : mWorldspace(std::move(worldspace))
        , mTileCachedRecastMeshManager(settings)
    {
        mAabb.m_min = btVector3(0, 0, 0);
        mAabb.m_max = btVector3(0, 0, 0);
    }

    WorldspaceData gatherWorldspaceData(const DetourNavigator::Settings& settings, std::vector<ESM::ESMReader>& readers,
        const VFS::Manager& vfs, Resource::BulletShapeManager& bulletShapeManager, const EsmLoader::EsmData& esmData,
        bool processInteriorCells)
    {
        Log(Debug::Info) << "Processing " << esmData.mCells.size() << " cells...";

        std::map<std::string_view, std::unique_ptr<WorldspaceNavMeshInput>> navMeshInputs;
        WorldspaceData data;

        std::size_t objectsCounter = 0;

        for (std::size_t i = 0; i < esmData.mCells.size(); ++i)
        {
            const ESM::Cell& cell = esmData.mCells[i];
            const bool exterior = cell.isExterior();

            if (!exterior && !processInteriorCells)
            {
                Log(Debug::Info) << "Skipped " << (exterior ? "exterior" : "interior")
                    << " cell (" << (i + 1) << "/" << esmData.mCells.size() << ") \"" << cell.getDescription() << "\"";
                continue;
            }

            Log(Debug::Debug) << "Processing " << (exterior ? "exterior" : "interior")
                << " cell (" << (i + 1) << "/" << esmData.mCells.size() << ") \"" << cell.getDescription() << "\"";

            const osg::Vec2i cellPosition(cell.mData.mX, cell.mData.mY);
            const std::size_t cellObjectsBegin = data.mObjects.size();

            WorldspaceNavMeshInput& navMeshInput = [&] () -> WorldspaceNavMeshInput&
            {
                auto it = navMeshInputs.find(cell.mCellId.mWorldspace);
                if (it == navMeshInputs.end())
                {
                    it = navMeshInputs.emplace(cell.mCellId.mWorldspace,
                        std::make_unique<WorldspaceNavMeshInput>(cell.mCellId.mWorldspace, settings.mRecast)).first;
                    it->second->mTileCachedRecastMeshManager.setWorldspace(cell.mCellId.mWorldspace);
                }
                return *it->second;
            } ();

            if (exterior)
            {
                const auto it = std::lower_bound(esmData.mLands.begin(), esmData.mLands.end(), cellPosition, LessByXY {});
                const auto [heightfieldShape, minHeight, maxHeight] = makeHeightfieldShape(
                    it == esmData.mLands.end() ? std::optional<ESM::Land>() : *it,
                    cellPosition, data.mHeightfields, data.mLandData
                );

                mergeOrAssign(getAabb(cellPosition, minHeight, maxHeight),
                              navMeshInput.mAabb, navMeshInput.mAabbInitialized);

                navMeshInput.mTileCachedRecastMeshManager.addHeightfield(cellPosition, ESM::Land::REAL_SIZE, heightfieldShape);

                navMeshInput.mTileCachedRecastMeshManager.addWater(cellPosition, ESM::Land::REAL_SIZE, -1);
            }
            else
            {
                if ((cell.mData.mFlags & ESM::Cell::HasWater) != 0)
                    navMeshInput.mTileCachedRecastMeshManager.addWater(cellPosition, std::numeric_limits<int>::max(), cell.mWater);
            }

            forEachObject(cell, esmData, vfs, bulletShapeManager, readers,
                [&] (BulletObject object)
                {
                    const btTransform& transform = object.getCollisionObject().getWorldTransform();
                    const btAABB aabb = BulletHelpers::getAabb(*object.getCollisionObject().getCollisionShape(), transform);
                    mergeOrAssign(aabb, navMeshInput.mAabb, navMeshInput.mAabbInitialized);
                    if (const btCollisionShape* avoid = object.getShapeInstance()->mAvoidCollisionShape.get())
                        navMeshInput.mAabb.merge(BulletHelpers::getAabb(*avoid, transform));

                    const ObjectId objectId(++objectsCounter);
                    const CollisionShape shape(object.getShapeInstance(), *object.getCollisionObject().getCollisionShape(), object.getObjectTransform());

                    navMeshInput.mTileCachedRecastMeshManager.addObject(objectId, shape, transform, DetourNavigator::AreaType_ground);

                    if (const btCollisionShape* avoid = object.getShapeInstance()->mAvoidCollisionShape.get())
                    {
                        const CollisionShape avoidShape(object.getShapeInstance(), *avoid, object.getObjectTransform());
                        navMeshInput.mTileCachedRecastMeshManager.addObject(objectId, avoidShape, transform, DetourNavigator::AreaType_null);
                    }

                    data.mObjects.emplace_back(std::move(object));
                });

            Log(Debug::Info) << "Processed " << (exterior ? "exterior" : "interior")
                << " cell (" << (i + 1) << "/" << esmData.mCells.size() << ") " << cell.getDescription()
                << " with " << (data.mObjects.size() - cellObjectsBegin) << " objects";
        }

        data.mNavMeshInputs.reserve(navMeshInputs.size());
        std::transform(navMeshInputs.begin(), navMeshInputs.end(), std::back_inserter(data.mNavMeshInputs),
                       [] (auto& v) { return std::move(v.second); });

        Log(Debug::Info) << "Processed " << esmData.mCells.size() << " cells, added "
            << data.mObjects.size() << " objects and " << data.mHeightfields.size() << " height fields";

        return data;
    }
}