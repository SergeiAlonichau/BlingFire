/**
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License.
 */


#ifndef _FA_DICTCONFKEEPER_H_
#define _FA_DICTCONFKEEPER_H_

#include "FAConfig.h"

class FALDB;
class FARSDfa_pack_triv;
class FAMealyDfa_pack_triv;
class FAState2Ow_pack_triv;
class FAArray_pack;
class FAMultiMap_pack;
class FAMultiMap_pack_mph;
class FARSDfaCA;
class FAMealyDfaCA;
class FAArrayCA;
class FAMultiMapCA;
class FAState2OwCA;
class FAMultiMap_pack_fixed;

///
/// Keeps dictionary object configuration and common containers.
///
/// Note: The pointers can be NULL, if yet not initialized
///

class FADictConfKeeper {

public:
    FADictConfKeeper ();
    ~FADictConfKeeper ();

public:
    /// this LDB will be used to get the data from
    void SetLDB (const FALDB * pLDB);
    /// initialization vector
    void Init (const int * pValues, const int Size);
    /// returns object into the initial state
    void Clear ();

public:
    const int GetFsmType () const;
    const FARSDfaCA * GetRsDfa () const;
    const FAMealyDfaCA * GetMphMealy () const;
    const FAState2OwCA * GetState2Ow () const;
    const FAArrayCA * GetK2I () const;
    const FAMultiMapCA * GetI2Info () const;
    const bool GetIgnoreCase () const;
    const bool GetNoTrUse () const;
    const int GetDirection () const;
    const FAMultiMapCA * GetCharMap () const;
    const int GetTokAlgo () const;

private:
    // input LDB
    const FALDB * m_pLDB;
    // W2K: Mealy-based MPH or Moore
    int m_FsmType;
    FARSDfa_pack_triv * m_pRsDfa;
    FAMealyDfa_pack_triv * m_pMealy;
    FAState2Ow_pack_triv * m_pState2Ow;
    // K2I: packed array
    FAArray_pack * m_pK2I;
    // I2Info: multi-map
    FAMultiMap_pack * m_pI2Info_triv;
    FAMultiMap_pack_mph * m_pI2Info_mph;
    FAMultiMap_pack_fixed * m_pI2Info_fixed;
    const FAMultiMapCA * m_pI2Info;
    // configuration options
    bool m_IgnoreCase;
    bool m_NoTrUse;
    int m_Direction;
    FAMultiMap_pack_fixed * m_pCharMap;
    // indicates what runtime algo to use with these data
    int m_TokAlgo;
};

#endif
