//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: 
//
//******************************************************************************

#ifndef INC_CBETSIZEINPUTBOX_H
#define INC_CBETSIZEINPUTBOX_H

class CBetsizeInputBox {
  friend class CBetSlider;
 public:
  CBetsizeInputBox();
  ~CBetsizeInputBox();
 public:
  bool EnterBetsize(double total_betsize_in_dollars);
  // Numpad-only amount entry (no keyboard, no i3edit/select): used after the
  // two-successive-clicks have opened the on-screen keypad. Adjusts the betsize,
  // clears with 5x nBackspace, clicks n0..n9 / nDecimalPoint, then clicks nOkay.
  bool EnterAmountViaNumpad(double total_betsize_in_dollars);
  // Like EnterAmountViaNumpad but types the amount AS-IS, skipping AdjustedBetsize
  // (min-raise / balance caps / "beautiful number" rounding). For the BB-only phone
  // keypad where the .ohf already gives the exact big-blind amount to type.
  bool EnterAmountViaNumpadRaw(double amount);
  // Depends on complete tablemap
  // and maybe visible betsize-confirmation-button
  bool IsReadyToBeUsed();
 protected:
  // To be used by the allin-slider
  bool Confirm();
 private:
  bool GetI3EditRegion();
 private:
  void SelectText();
  void Clear();
 private:
  // On-screen numpad entry (for phone/screen-scraped tables that can't take
  // keyboard input). Active when an "n0" region exists. Clears with 5x nBackspace,
  // then "types" the amount by clicking n0..n9 / nDecimalPoint region centres.
  bool UseNumpad();
  void EnterBetsizeByNumpad(CString amount);
  bool ClickNumpadRegion(CString region_name);
 private:
  // For future use
  bool VerifyEnteredBetsize();
 private:
  RECT _i3_edit_region;
  POINT p_null;
};

#endif INC_CBETSIZEINPUTBOX_H