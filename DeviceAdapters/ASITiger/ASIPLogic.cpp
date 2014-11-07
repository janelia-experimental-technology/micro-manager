///////////////////////////////////////////////////////////////////////////////
// FILE:          ASIPLogic.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   ASI programmable logic card device adapter
//
// COPYRIGHT:     Applied Scientific Instrumentation, Eugene OR
//
// LICENSE:       This file is distributed under the BSD license.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
//                IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//                CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//                INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
//
// AUTHOR:        Jon Daniels (jon@asiimaging.com) 10/2014
//
// BASED ON:      ASIStage.cpp and others
//

#ifdef WIN32
#define snprintf _snprintf 
#pragma warning(disable: 4355)
#endif

#include "ASIPLogic.h"
#include "ASITiger.h"
#include "ASIHub.h"
#include "../../MMDevice/ModuleInterface.h"
#include "../../MMDevice/DeviceUtils.h"
#include "../../MMDevice/DeviceBase.h"
#include "../../MMDevice/MMDevice.h"
#include <iostream>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#define PLOGIC_NUM_ADDRESSES     128
#define PLOGIC_INVERT_ADDRESS    64
#define PLOGIC_FRONTPANEL_START_ADDRESS  33
#define PLOGIC_FRONTPANEL_END_ADDRESS    40
#define PLOGIC_FRONTPANEL_NUM            (PLOGIC_FRONTPANEL_END_ADDRESS - PLOGIC_FRONTPANEL_START_ADDRESS + 1)
#define PLOGIC_BACKPLANE_START_ADDRESS   41
#define PLOGIC_BACKPLANE_END_ADDRESS     48
#define PLOGIC_BACKPLANE_NUM             (PLOGIC_BACKPLANE_END_ADDRESS - PLOGIC_BACKPLANE_START_ADDRESS + 1)
#define PLOGIC_PHYSICAL_IO_START_ADDRESS   PLOGIC_FRONTPANEL_START_ADDRESS
#define PLOGIC_PHYSICAL_IO_END_ADDRESS     PLOGIC_BACKPLANE_END_ADDRESS
#define PLOGIC_PHYSICAL_IO_NUM            (PLOGIC_PHYSICAL_IO_END_ADDRESS - PLOGIC_PHYSICAL_IO_START_ADDRESS + 1)

using namespace std;

// TODO order property names such that Micro-Manager can use presets (e.g. type must come before config)

///////////////////////////////////////////////////////////////////////////////
// CPLogic
//
CPLogic::CPLogic(const char* name) :
   ASIPeripheralBase< ::CGenericBase, CPLogic >(name),
   axisLetter_(g_EmptyAxisLetterStr),    // value determined by extended name
   numCells_(16),
   currentPosition_(1)
{
   if (IsExtendedName(name))  // only set up these properties if we have the required information in the name
   {
      axisLetter_ = GetAxisLetterFromExtName(name);
      CreateProperty(g_AxisLetterPropertyName, axisLetter_.c_str(), MM::String, true);
   }
}

int CPLogic::Initialize()
{
   // call generic Initialize first, this gets hub
   RETURN_ON_MM_ERROR( PeripheralInitialize() );

   // create MM description; this doesn't work during hardware configuration wizard but will work afterwards
   ostringstream command;
   command.str("");
   command << g_PLogicDeviceDescription << " HexAddr=" << addressString_;
   CreateProperty(MM::g_Keyword_Description, command.str().c_str(), MM::String, true);

   CPropertyAction* pAct;

   // try to detect the number of cells from the build name
   char buildName[MM::MaxStrLength];
   unsigned int tmp;
   GetProperty(g_FirmwareBuildPropertyName, buildName);
   string s = buildName;
   hub_->SetLastSerialAnswer(s);
   int ret = hub_->ParseAnswerAfterUnderscore(tmp);
   if (!ret) {
      numCells_ = tmp;
   }
   command.str("");
   command << numCells_;
   CreateProperty(g_NumLogicCellsPropertyName, command.str().c_str(), MM::Integer, true);

   // pointer position, this is where edits/queries are made in general
   pAct = new CPropertyAction (this, &CPLogic::OnPointerPosition);
   CreateProperty(g_PointerPositionPropertyName, "0", MM::Integer, false, pAct);
   UpdateProperty(g_PointerPositionPropertyName);

   // reports the output state of the logic cell array as unsigned integer
   pAct = new CPropertyAction (this, &CPLogic::OnPLogicOutputState);
   CreateProperty(g_PLogicOutputStatePropertyName, "0", MM::Integer, true, pAct);
   UpdateProperty(g_PLogicOutputStatePropertyName);

   // reports the output state of the BNCs as unsigned integer
   pAct = new CPropertyAction (this, &CPLogic::OnPLogicOutputState);
   CreateProperty(g_FrontpanelOutputStatePropertyName, "0", MM::Integer, true, pAct);
   UpdateProperty(g_FrontpanelOutputStatePropertyName);

   // reports the output state of the backplane IO as unsigned integer
   pAct = new CPropertyAction (this, &CPLogic::OnPLogicOutputState);
   CreateProperty(g_BackplaneOutputStatePropertyName, "0", MM::Integer, true, pAct);
   UpdateProperty(g_BackplaneOutputStatePropertyName);

   // sets the trigger source
   pAct = new CPropertyAction (this, &CPLogic::OnTriggerSource);
   CreateProperty(g_TriggerSourcePropertyName, "0", MM::String, false, pAct);
   AddAllowedValue(g_TriggerSourcePropertyName, g_SourceCode0, 0);
   AddAllowedValue(g_TriggerSourcePropertyName, g_SourceCode1, 1);
   AddAllowedValue(g_TriggerSourcePropertyName, g_SourceCode2, 2);
   AddAllowedValue(g_TriggerSourcePropertyName, g_SourceCode3, 3);
   AddAllowedValue(g_TriggerSourcePropertyName, g_SourceCode4, 4);
   UpdateProperty(g_TriggerSourcePropertyName);

   // refresh properties from controller every time; default is false = no refresh (speeds things up by not redoing so much serial comm)
   pAct = new CPropertyAction (this, &CPLogic::OnRefreshProperties);
   CreateProperty(g_RefreshPropValsPropertyName, g_NoState, MM::String, false, pAct);
   AddAllowedValue(g_RefreshPropValsPropertyName, g_NoState);
   AddAllowedValue(g_RefreshPropValsPropertyName, g_YesState);

   // save settings to controller if requested
   pAct = new CPropertyAction (this, &CPLogic::OnSaveCardSettings);
   CreateProperty(g_SaveSettingsPropertyName, g_SaveSettingsOrig, MM::String, false, pAct);
   AddAllowedValue(g_SaveSettingsPropertyName, g_SaveSettingsX);
   AddAllowedValue(g_SaveSettingsPropertyName, g_SaveSettingsY);
   AddAllowedValue(g_SaveSettingsPropertyName, g_SaveSettingsZ);
   AddAllowedValue(g_SaveSettingsPropertyName, g_SaveSettingsOrig);
   AddAllowedValue(g_SaveSettingsPropertyName, g_SaveSettingsDone);

   // generates a set of additional advanced properties that are used only rarely
   // in this case they allow configuring all the logic cells and setting outputs
   pAct = new CPropertyAction (this, &CPLogic::OnAdvancedProperties);
   CreateProperty(g_AdvancedPropertiesPropertyName, g_NoState, MM::String, false, pAct);
   UpdateProperty(g_AdvancedPropertiesPropertyName);
   AddAllowedValue(g_AdvancedPropertiesPropertyName, g_NoState);
   AddAllowedValue(g_AdvancedPropertiesPropertyName, g_YesState);

   initialized_ = true;
   return DEVICE_OK;
}



////////////////
// action handlers

int CPLogic::OnPLogicOutputState(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   unsigned int val;
   ostringstream command; command.str("");
   if (eAct == MM::BeforeGet || eAct == MM::AfterSet)
   {
      // always read
      command << addressChar_ << "RDADC Z?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(), ":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterPosition2(val) );
      if (!pProp->Set((long)val))
         return DEVICE_INVALID_PROPERTY_VALUE;
   }
   return DEVICE_OK;
}

int CPLogic::OnFrontpanelOutputState(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   unsigned int val;
   ostringstream command; command.str("");
   if (eAct == MM::BeforeGet || eAct == MM::AfterSet)
   {
      // always read
      command << addressChar_ << "RDADC X?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(), ":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterPosition2(val) );
      if (!pProp->Set((long)val))
         return DEVICE_INVALID_PROPERTY_VALUE;
   }
   return DEVICE_OK;
}

int CPLogic::OnBackplaneOutputState(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   unsigned int val;
   ostringstream command; command.str("");
   if (eAct == MM::BeforeGet || eAct == MM::AfterSet)
   {
      // always read
      command << addressChar_ << "RDADC Y?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(), ":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterPosition2(val) );
      if (!pProp->Set((long)val))
         return DEVICE_INVALID_PROPERTY_VALUE;
   }
   return DEVICE_OK;
}

int CPLogic::OnTriggerSource(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   ostringstream command; command.str("");
   long tmp;
   string tmpstr;
   if (eAct == MM::BeforeGet) {
      if (!refreshProps_ && initialized_)
         return DEVICE_OK;
      command << "PM " << axisLetter_ << "?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(), axisLetter_) );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterEquals(tmp) );
      bool success = 0;
      switch (tmp) {
         case 0: success = pProp->Set(g_SourceCode0); break;
         case 1: success = pProp->Set(g_SourceCode1); break;
         case 2: success = pProp->Set(g_SourceCode2); break;
         case 3: success = pProp->Set(g_SourceCode3); break;
         case 4: success = pProp->Set(g_SourceCode4); break;
         default: success=0;
      }
      if (!success)
         return DEVICE_INVALID_PROPERTY_VALUE;
   } else if (eAct == MM::AfterSet) {
      RETURN_ON_MM_ERROR ( GetCurrentPropertyData(g_TriggerSourcePropertyName, tmp) );
      command << "PM " << axisLetter_ << "=" << tmp;
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
   }
   return DEVICE_OK;
}

int CPLogic::OnPointerPosition(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   static bool justSet = false;
   ostringstream command; command.str("");
   if (eAct == MM::BeforeGet)
   {
      if (!refreshProps_ && initialized_ && !justSet)
         return DEVICE_OK;
      RefreshCurrentPosition();
      if (!pProp->Set((long)currentPosition_))
         return DEVICE_INVALID_PROPERTY_VALUE;
      justSet = false;
   } else  if (eAct == MM::AfterSet)
   {
      long val;
      pProp->Get(val);
      command << "M " << axisLetter_ << "=" << val;
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
      // read the result to make sure it happened
      justSet = true;
      return OnPointerPosition(pProp, MM::BeforeGet);
   }
   return DEVICE_OK;
}

int CPLogic::OnSaveCardSettings(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   string tmpstr;
   ostringstream command; command.str("");
   if (eAct == MM::AfterSet) {
      command << addressChar_ << "SS ";
      pProp->Get(tmpstr);
      if (tmpstr.compare(g_SaveSettingsOrig) == 0)
         return DEVICE_OK;
      if (tmpstr.compare(g_SaveSettingsDone) == 0)
         return DEVICE_OK;
      if (tmpstr.compare(g_SaveSettingsX) == 0)
         command << 'X';
      else if (tmpstr.compare(g_SaveSettingsY) == 0)
         command << 'X';
      else if (tmpstr.compare(g_SaveSettingsZ) == 0)
         command << 'Z';
      RETURN_ON_MM_ERROR (hub_->QueryCommandVerify(command.str(), ":A", (long)200));  // note 200ms delay added
      pProp->Set(g_SaveSettingsDone);
   }
   return DEVICE_OK;
}

int CPLogic::OnRefreshProperties(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   string tmpstr;
   if (eAct == MM::AfterSet) {
      pProp->Get(tmpstr);
      if (tmpstr.compare(g_YesState) == 0)
         refreshProps_ = true;
      else
         refreshProps_ = false;
   }
   return DEVICE_OK;
}

int CPLogic::GetCellPropertyName(long index, string suffix, char* name)
{
   ostringstream os;
   os << "Cell_" << setw(2) << setfill('0') << index << suffix;
   CDeviceUtils::CopyLimitedString(name, os.str().c_str());
   return DEVICE_OK;
}

int CPLogic::GetIOFrontpanelPropertyName(long index, char* name)
{
   ostringstream os;
   os << "SourceAddress_Frontpanel_" << index - PLOGIC_FRONTPANEL_START_ADDRESS + 1;
   CDeviceUtils::CopyLimitedString(name, os.str().c_str());
   return DEVICE_OK;
}

int CPLogic::GetIOBackplanePropertyName(long index, char* name)
{
   ostringstream os;
   os << "SourceAddress_Backplane_" << index - PLOGIC_BACKPLANE_START_ADDRESS;
   CDeviceUtils::CopyLimitedString(name, os.str().c_str());
   return DEVICE_OK;
}


int CPLogic::OnAdvancedProperties(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct == MM::BeforeGet)
   {
      return DEVICE_OK; // do nothing
   }
   else if (eAct == MM::AfterSet) {
      string tmpstr;
      pProp->Get(tmpstr);
      if (tmpstr.compare(g_YesState) == 0)
      {
         CPropertyActionEx* pActEx;
         char propName[MM::MaxStrLength];

         bool refreshPropsOriginal = refreshProps_;
         refreshProps_ = true;

         for (unsigned int i=1; i<=numCells_; i++) {

            // logic cell type
            GetCellPropertyName(i, "_Type", propName);
            pActEx = new CPropertyActionEx (this, &CPLogic::OnCellType, (long) i);
            CreateProperty(propName, g_TypeCode0, MM::String, false, pActEx);
            AddAllowedValue(propName, g_TypeCode0, 0);
            AddAllowedValue(propName, g_TypeCode1, 1);
            AddAllowedValue(propName, g_TypeCode2, 2);
            AddAllowedValue(propName, g_TypeCode3, 3);
            AddAllowedValue(propName, g_TypeCode4, 4);
            AddAllowedValue(propName, g_TypeCode5, 5);
            AddAllowedValue(propName, g_TypeCode6, 6);
            AddAllowedValue(propName, g_TypeCode7, 7);
            AddAllowedValue(propName, g_TypeCode8, 8);
            AddAllowedValue(propName, g_TypeCode9, 9);
            UpdateProperty(propName);

            // logic cell CCA Z code
            GetCellPropertyName(i, "_Config", propName);
            pActEx = new CPropertyActionEx (this, &CPLogic::OnCellConfig, (long) i);
            CreateProperty(propName, "0", MM::Integer, false, pActEx);
            UpdateProperty(propName);

            // logic cell input X code
            GetCellPropertyName(i, "_InputX", propName);
            pActEx = new CPropertyActionEx (this, &CPLogic::OnInputX, (long) i);
            CreateProperty(propName, "0", MM::Integer, false, pActEx);
            UpdateProperty(propName);

            // logic cell input Y code
            GetCellPropertyName(i, "_InputY", propName);
            pActEx = new CPropertyActionEx (this, &CPLogic::OnInputY, (long) i);
            CreateProperty(propName, "0", MM::Integer, false, pActEx);
            UpdateProperty(propName);

            // logic cell input Z code
            GetCellPropertyName(i, "_InputZ", propName);
            pActEx = new CPropertyActionEx (this, &CPLogic::OnInputZ, (long) i);
            CreateProperty(propName, "0", MM::Integer, false, pActEx);
            UpdateProperty(propName);

            // logic cell input F code
            GetCellPropertyName(i, "_InputF", propName);
            pActEx = new CPropertyActionEx (this, &CPLogic::OnInputF, (long) i);
            CreateProperty(propName, "0", MM::Integer, false, pActEx);
            UpdateProperty(propName);

         }

         for (unsigned int i=PLOGIC_FRONTPANEL_START_ADDRESS; i<=PLOGIC_FRONTPANEL_END_ADDRESS; i++) {
            GetIOFrontpanelPropertyName(i, propName);
            pActEx = new CPropertyActionEx (this, &CPLogic::OnIOSourceAddress, (long) i);
            CreateProperty(propName, "0", MM::Integer, false, pActEx);
            UpdateProperty(propName);
         }

         for (unsigned int i=PLOGIC_BACKPLANE_START_ADDRESS; i<=PLOGIC_BACKPLANE_END_ADDRESS; i++) {
            GetIOBackplanePropertyName(i, propName);
            pActEx = new CPropertyActionEx (this, &CPLogic::OnIOSourceAddress, (long) i);
            CreateProperty(propName, "0", MM::Integer, false, pActEx);
            UpdateProperty(propName);
         }

         refreshProps_ = refreshPropsOriginal;
      }
   }
   return DEVICE_OK;
}

int CPLogic::RefreshCellPropertyValues(long index)
{
   char propName[MM::MaxStrLength];
   bool refreshPropsOriginal = refreshProps_;
   refreshProps_ = true;

   GetCellPropertyName(index, "_Config", propName);
   UpdateProperty(propName);
   GetCellPropertyName(index, "_InputX", propName);
   UpdateProperty(propName);
   GetCellPropertyName(index, "_InputY", propName);
   UpdateProperty(propName);
   GetCellPropertyName(index, "_InputZ", propName);
   UpdateProperty(propName);
   GetCellPropertyName(index, "_InputF", propName);
   UpdateProperty(propName);

   refreshProps_ = refreshPropsOriginal;
   return DEVICE_OK;
}

int CPLogic::SetPosition(unsigned int position)
{
   ostringstream command; command.str("");
   if (position == currentPosition_)
      return DEVICE_OK;
   command << "M " << axisLetter_ << "=" << position;
   RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
   currentPosition_ = position;
   command.str("");
   command << position;
   SetProperty(g_PointerPositionPropertyName, command.str().c_str());
   return DEVICE_OK;
}

int CPLogic::RefreshCurrentPosition()
{
   ostringstream command; command.str("");
   unsigned int tmp;
   command << "W " << axisLetter_;
   RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
   RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterPosition2(tmp) );
   currentPosition_ = tmp;
   return DEVICE_OK;
}

int CPLogic::OnCellType(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   ostringstream command; command.str("");
   long tmp;
   string tmpstr;
   if (eAct == MM::BeforeGet) {
      if (!refreshProps_ && initialized_)
         return DEVICE_OK;
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCA Y?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterEquals(tmp) );
      bool success = 0;
      switch (tmp) {
         case 0: success = pProp->Set(g_TypeCode0); break;
         case 1: success = pProp->Set(g_TypeCode1); break;
         case 2: success = pProp->Set(g_TypeCode2); break;
         case 3: success = pProp->Set(g_TypeCode3); break;
         case 4: success = pProp->Set(g_TypeCode4); break;
         case 5: success = pProp->Set(g_TypeCode5); break;
         case 6: success = pProp->Set(g_TypeCode6); break;
         case 7: success = pProp->Set(g_TypeCode7); break;
         case 8: success = pProp->Set(g_TypeCode8); break;
         case 9: success = pProp->Set(g_TypeCode9); break;
         default: success=0;
      }
      if (!success)
         return DEVICE_INVALID_PROPERTY_VALUE;
   } else if (eAct == MM::AfterSet) {
      char propName[MM::MaxStrLength];
      GetCellPropertyName(index, "_Type", propName);
      RETURN_ON_MM_ERROR ( GetCurrentPropertyData(propName, tmp) );
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCA Y=" << tmp;
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
      RETURN_ON_MM_ERROR ( RefreshCellPropertyValues(index) );
   }
   return DEVICE_OK;
}

int CPLogic::OnCellConfig(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   ostringstream command; command.str("");
   long tmp;
   if (eAct == MM::BeforeGet) {
      if (!refreshProps_ && initialized_)
         return DEVICE_OK;
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCA Z?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterEquals(tmp) );
      if (!pProp->Set(tmp))
         return DEVICE_INVALID_PROPERTY_VALUE;
   } else if (eAct == MM::AfterSet) {
      pProp->Get(tmp);
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCA Z=" << tmp;
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
   }
   return DEVICE_OK;
}

int CPLogic::OnInputX(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   ostringstream command; command.str("");
   long tmp;
   if (eAct == MM::BeforeGet) {
      if (!refreshProps_ && initialized_)
         return DEVICE_OK;
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCB X?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterEquals(tmp) );
      if (!pProp->Set(tmp))
         return DEVICE_INVALID_PROPERTY_VALUE;
   } else if (eAct == MM::AfterSet) {
      pProp->Get(tmp);
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCB X=" << tmp;
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
   }
   return DEVICE_OK;
}

int CPLogic::OnInputY(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   ostringstream command; command.str("");
   long tmp;
   if (eAct == MM::BeforeGet) {
      if (!refreshProps_ && initialized_)
         return DEVICE_OK;
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCB Y?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterEquals(tmp) );
      if (!pProp->Set(tmp))
         return DEVICE_INVALID_PROPERTY_VALUE;
   } else if (eAct == MM::AfterSet) {
      pProp->Get(tmp);
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCB Y=" << tmp;
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
   }
   return DEVICE_OK;
}

int CPLogic::OnInputZ(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   ostringstream command; command.str("");
   long tmp;
   if (eAct == MM::BeforeGet) {
      if (!refreshProps_ && initialized_)
         return DEVICE_OK;
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCB Z?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterEquals(tmp) );
      if (!pProp->Set(tmp))
         return DEVICE_INVALID_PROPERTY_VALUE;
   } else if (eAct == MM::AfterSet) {
      pProp->Get(tmp);
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCB Z=" << tmp;
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
   }
   return DEVICE_OK;
}

int CPLogic::OnInputF(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   ostringstream command; command.str("");
   long tmp;
   if (eAct == MM::BeforeGet) {
      if (!refreshProps_ && initialized_)
         return DEVICE_OK;
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCB F?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterEquals(tmp) );
      if (!pProp->Set(tmp))
         return DEVICE_INVALID_PROPERTY_VALUE;
   } else if (eAct == MM::AfterSet) {
      pProp->Get(tmp);
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCB F=" << tmp;
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
   }
   return DEVICE_OK;
}

int CPLogic::OnIOSourceAddress(MM::PropertyBase* pProp, MM::ActionType eAct, long index)
{
   ostringstream command; command.str("");
   long tmp;
   if (eAct == MM::BeforeGet) {
      if (!refreshProps_ && initialized_)
         return DEVICE_OK;
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCA Z?";
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
      RETURN_ON_MM_ERROR ( hub_->ParseAnswerAfterEquals(tmp) );
      if (!pProp->Set(tmp))
         return DEVICE_INVALID_PROPERTY_VALUE;
   } else if (eAct == MM::AfterSet) {
      pProp->Get(tmp);
      RETURN_ON_MM_ERROR ( SetPosition(index) );
      command << addressChar_ << "CCA Z=" << tmp;
      RETURN_ON_MM_ERROR ( hub_->QueryCommandVerify(command.str(),":A") );
   }
   return DEVICE_OK;
}

