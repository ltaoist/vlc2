/* comment */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <graphic/Manager.hxx>
#include <impgraph.hxx>
#include <sal/log.hxx>

#include <officecfg/Office/Common.hxx>
#include <unotools/configmgr.hxx>

using namespace css;

namespace vcl::graphic
{
namespace
{
void setupConfigurationValuesIfPossible(sal_Int64& rMemoryLimit,
                                        std::chrono::seconds& rAllowedIdleTime, bool& bSwapEnabled)
{
    if (utl::ConfigManager::IsFuzzing())
        return;

    try
    {
        using officecfg::Office::Common::Cache;

        rMemoryLimit = Cache::GraphicManager::GraphicMemoryLimit::get();
        rAllowedIdleTime
            = std::chrono::seconds(Cache::GraphicManager::GraphicAllowedIdleTime::get());
        bSwapEnabled = Cache::GraphicManager::GraphicSwappingEnabled::get();
    }
    catch (...)
    {
    }
}
}

Manager& Manager::get()
{
    static Manager gStaticManager;
    return gStaticManager;
}

Manager::Manager()
    : mnAllowedIdleTime(10)
    , mbSwapEnabled(true)
    , mbReducingGraphicMemory(false)
    , mnMemoryLimit(300000000)
    , mnUsedSize(0)
    , maSwapOutTimer("graphic::Manager maSwapOutTimer")
{
    setupConfigurationValuesIfPossible(mnMemoryLimit, mnAllowedIdleTime, mbSwapEnabled);

    if (mbSwapEnabled)
    {
        maSwapOutTimer.SetInvokeHandler(LINK(this, Manager, SwapOutTimerHandler));
        maSwapOutTimer.SetTimeout(10000);
        maSwapOutTimer.Start();
    }
}

void Manager::loopGraphicsAndSwapOut(std::unique_lock<std::mutex>& rGuard)
{
    // make a copy of m_pImpGraphicList because if we swap out a svg, the svg
    // filter may create more temp Graphics which are auto-added to
    // m_pImpGraphicList invalidating a loop over m_pImpGraphicList, e.g.
    // reexport of tdf118346-1.odg
    o3tl::sorted_vector<ImpGraphic*> aImpGraphicList = m_pImpGraphicList;

    for (ImpGraphic* pEachImpGraphic : aImpGraphicList)
    {
        if (mnUsedSize < sal_Int64(mnMemoryLimit * 0.7))
            return;

        if (pEachImpGraphic->isSwappedOut())
            continue;

        sal_Int64 nCurrentGraphicSize = getGraphicSizeBytes(pEachImpGraphic);
        if (nCurrentGraphicSize > 100000)
        {
            if (!pEachImpGraphic->mpContext)
            {
                auto aCurrent = std::chrono::high_resolution_clock::now();
                auto aDeltaTime = aCurrent - pEachImpGraphic->maLastUsed;
                auto aSeconds = std::chrono::duration_cast<std::chrono::seconds>(aDeltaTime);

                if (aSeconds > mnAllowedIdleTime)
                {
                    // unlock because svgio can call back into us
                    rGuard.unlock();
                    pEachImpGraphic->swapOut();
                    rGuard.lock();
                }
            }
        }
    }
}

void Manager::reduceGraphicMemory(std::unique_lock<std::mutex>& rGuard)
{
    // maMutex is locked in callers

    if (!mbSwapEnabled)
        return;

    if (mnUsedSize < mnMemoryLimit)
        return;

    // avoid recursive reduceGraphicMemory on reexport of tdf118346-1.odg to odg
    if (mbReducingGraphicMemory)
        return;
    mbReducingGraphicMemory = true;

    loopGraphicsAndSwapOut(rGuard);

    sal_Int64 calculatedSize = 0;
    for (ImpGraphic* pEachImpGraphic : m_pImpGraphicList)
    {
        if (!pEachImpGraphic->isSwappedOut())
        {
            calculatedSize += getGraphicSizeBytes(pEachImpGraphic);
        }
    }

    if (calculatedSize != mnUsedSize)
    {
        assert(rGuard.owns_lock() && rGuard.mutex() == &maMutex);
        // coverity[missing_lock: FALSE] - as above assert
        mnUsedSize = calculatedSize;
    }

    mbReducingGraphicMemory = false;
}

sal_Int64 Manager::getGraphicSizeBytes(const ImpGraphic* pImpGraphic)
{
    if (!pImpGraphic->isAvailable())
        return 0;
    return pImpGraphic->getSizeBytes();
}

IMPL_LINK(Manager, SwapOutTimerHandler, Timer*, pTimer, void)
{
    std::unique_lock aGuard(maMutex);

    pTimer->Stop();
    reduceGraphicMemory(aGuard);
    pTimer->Start();
}

void Manager::registerGraphic(const std::shared_ptr<ImpGraphic>& pImpGraphic)
{
    std::unique_lock aGuard(maMutex);

    // make some space first
    if (mnUsedSize > mnMemoryLimit)
        reduceGraphicMemory(aGuard);

    // Insert and update the used size (bytes)
    assert(aGuard.owns_lock() && aGuard.mutex() == &maMutex);
    // coverity[missing_lock: FALSE] - as above assert
    mnUsedSize += getGraphicSizeBytes(pImpGraphic.get());
    m_pImpGraphicList.insert(pImpGraphic.get());

    // calculate size of the graphic set
    sal_Int64 calculatedSize = 0;
    for (ImpGraphic* pEachImpGraphic : m_pImpGraphicList)
    {
        if (!pEachImpGraphic->isSwappedOut())
        {
            calculatedSize += getGraphicSizeBytes(pEachImpGraphic);
        }
    }

    if (calculatedSize != mnUsedSize)
    {
        SAL_INFO_IF(calculatedSize != mnUsedSize, "vcl.gdi",
                    "Calculated size mismatch. Variable size is '"
                        << mnUsedSize << "' but calculated size is '" << calculatedSize << "'");
        mnUsedSize = calculatedSize;
    }
}

void Manager::unregisterGraphic(ImpGraphic* pImpGraphic)
{
    std::scoped_lock aGuard(maMutex);

    mnUsedSize -= getGraphicSizeBytes(pImpGraphic);
    m_pImpGraphicList.erase(pImpGraphic);
}

std::shared_ptr<ImpGraphic> Manager::copy(std::shared_ptr<ImpGraphic> const& rImpGraphicPtr)
{
    auto pReturn = std::make_shared<ImpGraphic>(*rImpGraphicPtr);
    registerGraphic(pReturn);
    return pReturn;
}

std::shared_ptr<ImpGraphic> Manager::newInstance()
{
    auto pReturn = std::make_shared<ImpGraphic>();
    registerGraphic(pReturn);
    return pReturn;
}

std::shared_ptr<ImpGraphic> Manager::newInstance(std::shared_ptr<GfxLink> const& rGfxLink,
                                                 sal_Int32 nPageIndex)
{
    auto pReturn = std::make_shared<ImpGraphic>(rGfxLink, nPageIndex);
    registerGraphic(pReturn);
    return pReturn;
}

std::shared_ptr<ImpGraphic> Manager::newInstance(const BitmapEx& rBitmapEx)
{
    auto pReturn = std::make_shared<ImpGraphic>(rBitmapEx);
    registerGraphic(pReturn);
    return pReturn;
}

std::shared_ptr<ImpGraphic> Manager::newInstance(const Animation& rAnimation)
{
    auto pReturn = std::make_shared<ImpGraphic>(rAnimation);
    registerGraphic(pReturn);
    return pReturn;
}

std::shared_ptr<ImpGraphic>
Manager::newInstance(const std::shared_ptr<VectorGraphicData>& rVectorGraphicDataPtr)
{
    auto pReturn = std::make_shared<ImpGraphic>(rVectorGraphicDataPtr);
    registerGraphic(pReturn);
    return pReturn;
}

std::shared_ptr<ImpGraphic> Manager::newInstance(const GDIMetaFile& rMetaFile)
{
    auto pReturn = std::make_shared<ImpGraphic>(rMetaFile);
    registerGraphic(pReturn);
    return pReturn;
}

std::shared_ptr<ImpGraphic> Manager::newInstance(const GraphicExternalLink& rGraphicLink)
{
    auto pReturn = std::make_shared<ImpGraphic>(rGraphicLink);
    registerGraphic(pReturn);
    return pReturn;
}

void Manager::swappedIn(const ImpGraphic* pImpGraphic, sal_Int64 nSizeBytes)
{
    std::scoped_lock aGuard(maMutex);
    if (pImpGraphic)
    {
        mnUsedSize += nSizeBytes;
    }
}

void Manager::swappedOut(const ImpGraphic* pImpGraphic, sal_Int64 nSizeBytes)
{
    std::scoped_lock aGuard(maMutex);
    if (pImpGraphic)
    {
        mnUsedSize -= nSizeBytes;
    }
}

void Manager::changeExisting(const ImpGraphic* pImpGraphic, sal_Int64 nOldSizeBytes)
{
    std::scoped_lock aGuard(maMutex);

    mnUsedSize -= nOldSizeBytes;
    mnUsedSize += getGraphicSizeBytes(pImpGraphic);
}
} // end vcl::graphic

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
