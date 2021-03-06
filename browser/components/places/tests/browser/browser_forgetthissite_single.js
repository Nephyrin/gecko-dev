/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This test makes sure that the Forget This Site command is hidden for multiple
// selections.
function test() {
  // initialization
  waitForExplicitFinish();

  // Add a history entry.
  let TEST_URIs = ["http://www.mozilla.org/test1", "http://www.mozilla.org/test2"];
  ok(PlacesUtils, "checking PlacesUtils, running in chrome context?");
  let places = [];
  TEST_URIs.forEach(function(TEST_URI) {
    places.push({uri: PlacesUtils._uri(TEST_URI),
                 transition: PlacesUtils.history.TRANSITION_TYPED});
  });
  addVisits(places, window, function() {
    testForgetThisSiteVisibility(1, function() {
      testForgetThisSiteVisibility(2, function() {
        // Cleanup
        waitForClearHistory(finish);
      });
    });
  });

  function testForgetThisSiteVisibility(selectionCount, funcNext) {
    openLibrary(function (organizer) {
          // Select History in the left pane.
          organizer.PlacesOrganizer.selectLeftPaneQuery('History');
          let PO = organizer.PlacesOrganizer;
          let histContainer = PO._places.selectedNode.QueryInterface(Ci.nsINavHistoryContainerResultNode);
          histContainer.containerOpen = true;
          PO._places.selectNode(histContainer.getChild(0));
          // Select the first history entry.
          let doc = organizer.document;
          let tree = PO._content;
          let selection = tree.view.selection;
          selection.clearSelection();
          selection.rangedSelect(0, selectionCount - 1, true);
          is(selection.count, selectionCount,
            "The selected range is as big as expected");
          // Open the context menu
          let contextmenu = doc.getElementById("placesContext");
          contextmenu.addEventListener("popupshown", function() {
            contextmenu.removeEventListener("popupshown", arguments.callee, true);
            let forgetThisSite = doc.getElementById("placesContext_deleteHost");
            let hideForgetThisSite = (selectionCount != 1);
            is(forgetThisSite.hidden, hideForgetThisSite,
              "The Forget this site menu item should " + (hideForgetThisSite ? "" : "not ") +
              "be hidden with " + selectionCount + " items selected");
            // Close the context menu
            contextmenu.hidePopup();
            // Wait for the Organizer window to actually be closed
            organizer.addEventListener("unload", function () {
              organizer.removeEventListener("unload", arguments.callee, false);
              // Proceed
              funcNext();
            }, false);
            // Close Library window.
            organizer.close();
          }, true);
          // Get cell coordinates
          var rect = tree.treeBoxObject.getCoordsForCellItem(0, tree.columns[0], "text");
          // Initiate a context menu for the selected cell
          EventUtils.synthesizeMouse(tree.body, rect.x + rect.width / 2, rect.y + rect.height / 2, {type: "contextmenu"}, organizer);
    });
  }
}
