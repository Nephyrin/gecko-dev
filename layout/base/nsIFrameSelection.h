/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation. All
 * Rights Reserved.
 *
 * Contributor(s): 
 */

/*
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * NOTE!!  This is not a general class, but specific to layout and frames.
 * Consumers looking for the general selection interface should look at
 * nsIDOMSelection.
 */

#ifndef nsIFrameSelection_h___
#define nsIFrameSelection_h___

#include "nsISupports.h"
#include "nsIFrame.h"
#include "nsIFocusTracker.h"   
#include "nsIDOMSelection.h"
#include "nsIPresShell.h"
#include "nsIContent.h"
#include "nsCOMPtr.h"
#include "nsIStyleContext.h"
#include "nsISelectionController.h"


// IID for the nsIFrameSelection interface
#define NS_IFRAMESELECTION_IID      \
{ 0xf46e4171, 0xdeaa, 0x11d1, \
  { 0x97, 0xfc, 0x0, 0x60, 0x97, 0x3, 0xc1, 0x4e } }


//----------------------------------------------------------------------

// Selection interface

struct SelectionDetails
{
  PRInt32 mStart;
  PRInt32 mEnd;
  SelectionType mType;
  SelectionDetails *mNext;
};

/*PeekOffsetStruct
   *  @param mTracker is used to get the PresContext usefull for measuring text ect.
   *  @param mDesiredX is the "desired" location of the new caret
   *  @param mAmount eWord, eCharacter, eLine
   *  @param mDirection enum defined in this file to be eForward or eBackward
   *  @param mStartOffset start offset to start the peek. 0 == beginning -1 = end
   *  @param mResultContent content that actually is the next/previous
   *  @param mResultOffset offset for result content
   *  @param mResultFrame resulting frame for peeking
   *  @param mEatingWS boolean to tell us the state of our search for Next/Prev
   *  @param mPreferLeft true = prev line end, false = next line begin
   *  @param mJumpLines if this is true then its ok to cross lines while peeking
*/
struct nsPeekOffsetStruct
{
  void SetData(nsIFocusTracker *aTracker, 
               nscoord aDesiredX, 
               nsSelectionAmount aAmount,
               nsDirection aDirection,
               PRInt32 aStartOffset, 
               PRBool aEatingWS,
               PRBool aPreferLeft,
               PRBool aJumpLines)
      {
       mTracker=aTracker;mDesiredX=aDesiredX;mAmount=aAmount;
       mDirection=aDirection;mStartOffset=aStartOffset;mEatingWS=aEatingWS;
       mPreferLeft=aPreferLeft;mJumpLines = aJumpLines;
      }
  nsIFocusTracker *mTracker;
  nscoord mDesiredX;
  nsSelectionAmount mAmount;
  nsDirection mDirection;
  PRInt32 mStartOffset;
  nsCOMPtr<nsIContent> mResultContent;
  PRInt32 mContentOffset;
  PRInt32 mContentOffsetEnd;
  nsIFrame *mResultFrame;
  PRBool mEatingWS;
  PRBool mPreferLeft;
  PRBool mJumpLines;
};

// Values for aFlag parameter in HandleTableSelection
enum {
  TABLESELECTION_CELL=1,
  TABLESELECTION_ROW,
  TABLESELECTION_COLUMN,
  TABLESELECTION_TABLE,
  TABLESELECTION_ALLCELLS
};
  
class nsIFrameSelection : public nsISupports {
public:
  static const nsIID& GetIID() { static nsIID iid = NS_IFRAMESELECTION_IID; return iid; }

  /** Init will initialize the frame selector with the necessary focus tracker to 
   *  be used by most of the methods
   *  @param aTracker is the parameter to be used for most of the other calls for callbacks ect
   */
  NS_IMETHOD Init(nsIFocusTracker *aTracker) = 0;

  /** ShutDown will be called when the owner of the frame selection is shutting down
   *  this should be the time to release all member variable interfaces. all methods
   *  called after ShutDown should return NS_ERROR_FAILURE
   */
  NS_IMETHOD ShutDown() = 0;

  /** HandleKeyEvent will accept an event and frame and 
   *  will return NS_OK if it handles the event or NS_COMFALSE if not.
   *  <P>DOES NOT ADDREF<P>
   *  @param aGuiEvent is the event that should be dealt with by aFocusFrame
   *  @param aFrame is the frame that MAY handle the event
   */
  NS_IMETHOD HandleTextEvent(nsGUIEvent *aGuiEvent) = 0;

  /** HandleKeyEvent will accept an event and frame and 
   *  will return NS_OK if it handles the event or NS_COMFALSE if not.
   *  <P>DOES NOT ADDREF<P>
   *  @param aGuiEvent is the event that should be dealt with by aFocusFrame
   *  @param aFrame is the frame that MAY handle the event
   */
  NS_IMETHOD HandleKeyEvent(nsIPresContext* aPresContext, nsGUIEvent *aGuiEvent) = 0;

  /** HandleClick will take the focus to the new frame at the new offset and 
   *  will either extend the selection from the old anchor, or replace the old anchor.
   *  the old anchor and focus position may also be used to deselect things
   *  @param aNewfocus is the content that wants the focus
   *  @param aContentOffset is the content offset of the parent aNewFocus
   *  @param aContentOffsetEnd is the content offset of the parent aNewFocus and is specified different
   *                           when you need to select to and include both start and end points
   *  @param aContinueSelection is the flag that tells the selection to keep the old anchor point or not.
   *  @param aMultipleSelection will tell the frame selector to replace /or not the old selection. 
   *         cannot coexist with aContinueSelection
   *  @param aHint will tell the selection which direction geometrically to actually show the caret on. 
   *         1 = end of this line 0 = beggining of this line
   */
  NS_IMETHOD HandleClick(nsIContent *aNewFocus, PRUint32 aContentOffset, PRUint32 aContentEndOffset , 
                       PRBool aContinueSelection, PRBool aMultipleSelection, PRBool aHint) = 0; 

  /** HandleDrag extends the selection to contain the frame closest to aPoint.
   *  @param aPresContext is the context to use when figuring out what frame contains the point.
   *  @param aFrame is the parent of all frames to use when searching for the closest frame to the point.
   *  @param aPoint is relative to aFrame's parent view.
   */
  NS_IMETHOD HandleDrag(nsIPresContext *aPresContext, nsIFrame *aFrame, nsPoint& aPoint) = 0;

  /** HandleTableSelection will set selection to a table, cell, etc
   *   depending on information contained in aFlags
   *  @param aParentContent is the paretent of either a table or cell that user clicked or dragged the mouse in
   *  @param aContentOffset is the offset of the table or cell
   *  @param aTarget indicates what to select:
   *    TABLESELECTION_CELL      We should select a cell (content points to the cell)
   *    TABLESELECTION_ROW       We should select a row (content points to first cell in row)
   *    TABLESELECTION_COLUMN    We should select a row (content points to first cell in column)
   *    TABLESELECTION_TABLE     We should select a table (content points to the table)
   *    TABLESELECTION_ALLCELLS  We should select all cells (content points to first cell in table)
   *  @param aMouseEvent         passed in so we we can get where event occured and what keys are pressed
   */
  NS_IMETHOD HandleTableSelection(nsIContent *aParentContent, PRInt32 aContentOffset, PRUint32 aTarget, nsMouseEvent *aMouseEvent) = 0;

  /** StartAutoScrollTimer is responsible for scrolling the view so that aPoint is always
   *  visible, and for selecting any frame that contains aPoint. The timer will also reset
   *  itself to fire again if the view has not scrolled to the end of the document.
   *  @param aPresContext is the context to use when figuring out what frame contains the point.
   *  @param aFrame is the parent of all frames to use when searching for the closest frame to the point.
   *  @param aPoint is relative to aFrame's parent view.
   *  @param aDelay is the timer's interval.
   */
  NS_IMETHOD StartAutoScrollTimer(nsIPresContext *aPresContext, nsIFrame *aFrame, nsPoint& aPoint, PRUint32 aDelay) = 0;

  /** StopAutoScrollTimer stops any active auto scroll timer.
   */
  NS_IMETHOD StopAutoScrollTimer() = 0;

  /** EnableFrameNotification
   *  mutch like start batching, except all dirty calls are ignored. no notifications will go 
   *  out until enableNotifications with a PR_TRUE is called
   */
  NS_IMETHOD EnableFrameNotification(PRBool aEnable) = 0;

  /** Lookup Selection
   *  returns in frame coordinates the selection beginning and ending with the type of selection given
   * @param aContent is the content asking
   * @param aContentOffset is the starting content boundary
   * @param aContentLength is the length of the content piece asking
   * @param aReturnDetails linkedlist of return values for the selection. 
   * @param aSlowCheck will check using slow method with no shortcuts
   */
  NS_IMETHOD LookUpSelection(nsIContent *aContent, PRInt32 aContentOffset, PRInt32 aContentLength,
                             SelectionDetails **aReturnDetails, PRBool aSlowCheck) = 0;

  /** SetMouseDownState(PRBool);
   *  sets the mouse state to aState for resons of drag state.
   * @param aState is the new state of mousedown
   */
  NS_IMETHOD SetMouseDownState(PRBool aState)=0;

  /** GetMouseDownState(PRBool *);
   *  gets the mouse state to aState for resons of drag state.
   * @param aState will hold the state of mousedown
   */
  NS_IMETHOD GetMouseDownState(PRBool *aState)=0;

  /**
    if we are in table cell selection mode. aka ctrl click in table cell
   */
  NS_IMETHOD GetTableCellSelection(PRBool *aState)=0;

  /** GetTableCellSelectionStyleColor
   *  this holds the color of the selection for table cells when they are selected.
   */
  NS_IMETHOD GetTableCellSelectionStyleColor(const nsStyleColor **aStyleColor)=0;

  /** GetSelection
   * no query interface for selection. must use this method now.
   * @param aSelectionType enum value defined in nsIDOMSelection for the domseleciton you want.
   */
  NS_IMETHOD GetSelection(SelectionType aSelectionType, nsIDOMSelection **aDomSelection)=0;

  /**
   * ScrollSelectionIntoView scrolls a region of the selection,
   * so that it is visible in the scrolled view.
   *
   * @param aType the selection to scroll into view.
   * @param aRegion the region inside the selection to scroll into view.
   */
  NS_IMETHOD ScrollSelectionIntoView(SelectionType aSelectionType, SelectionRegion aRegion)=0;

  /** RepaintSelection repaints the selected frames that are inside the selection
   *  specified by aSelectionType.
   * @param aSelectionType enum value defined in nsIDOMSelection for the domseleciton you want.
   */
  NS_IMETHOD RepaintSelection(nsIPresContext* aPresContext, SelectionType aSelectionType)=0;

  /** GetFrameForNodeOffset given a node and its child offset, return the nsIFrame and
   *  the offset into that frame. 
   * @param aNode input parameter for the node to look at
   * @param aOffset offset into above node.
   * @param aReturnFrame will contain the return frame. MUST NOT BE NULL or will return error
   * @param aReturnOffset will contain offset into frame.
   */
  NS_IMETHOD GetFrameForNodeOffset(nsIContent *aNode, PRInt32 aOffset, nsIFrame **aReturnFrame, PRInt32 *aReturnOffset)=0;

  /** CharacterMove will generally be called from the nsiselectioncontroller implementations.
   *  the effect being the selection will move one character left or right.
   * @param aForward move forward in document.
   * @param aExtend continue selection
   */
  NS_IMETHOD CharacterMove(PRBool aForward, PRBool aExtend)=0;

  /** WordMove will generally be called from the nsiselectioncontroller implementations.
   *  the effect being the selection will move one word left or right.
   * @param aForward move forward in document.
   * @param aExtend continue selection
   */
  NS_IMETHOD WordMove(PRBool aForward, PRBool aExtend)=0;

  /** LineMove will generally be called from the nsiselectioncontroller implementations.
   *  the effect being the selection will move one line up or down.
   * @param aForward move forward in document.
   * @param aExtend continue selection
   */
  NS_IMETHOD LineMove(PRBool aForward, PRBool aExtend)=0;

  /** IntraLineMove will generally be called from the nsiselectioncontroller implementations.
   *  the effect being the selection will move to beginning or end of line
   * @param aForward move forward in document.
   * @param aExtend continue selection
   */
  NS_IMETHOD IntraLineMove(PRBool aForward, PRBool aExtend)=0;

  /** Select All will generally be called from the nsiselectioncontroller implementations.
   *  it will select the whole doc
   */
  NS_IMETHOD SelectAll()=0;
};



#endif /* nsIFrameSelection_h___ */
