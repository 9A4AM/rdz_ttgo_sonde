let stypes=new Map();
stypes.set('4', 'RS41');
stypes.set('R', 'RS92');
stypes.set('D', 'DFM');
stypes.set('M', 'M10/M20');
stypes.set('3', 'MP3H');

function loadaprs(baseurl,callback) {
  var link = document.createElement('link');
  link.rel = 'stylesheet';
  link.href = baseurl + "/aprs-symbols.css";
  document.head.appendChild(link);
  var script = document.createElement('script');
  script.src = baseurl + "/aprs-symbols.js";
  script.onload = function() { if (typeof callback === 'function') { callback(); } }
  document.head.appendChild(script);
}

document.addEventListener('DOMContentLoaded', function() {
 loadaprs("http://rdzsonde.mooo.com/aprs", function() {
  var inputBox = document.querySelector('input[name="tcp.beaconsym"]');
  if(inputBox) {
    inputBox.addEventListener('input', showaprs);
    var newdiv = document.createElement('div');
    newdiv.id = 'aprsSyms';
    inputBox.insertAdjacentElement('afterend', newdiv);
    function showaprs() {
      var inp = inputBox.value;
      var tag1 = getAPRSSymbolImageTag(inp.slice(0,2));
      var tag2 = getAPRSSymbolImageTag(inp.slice(2,4));
      if(typeof tag1 === 'string') { newdiv.innerHTML = "Fixed: "+tag1; }
      if(typeof tag2 === 'string') { newdiv.innerHTML += " Chase: "+tag2; }
    }
    showaprs();
    inputBox.addEventListener('input', function() { showaprs(); });
  }
 });
});
  
function footer() {
  document.addEventListener("DOMContentLoaded", function(){
    var form = document.querySelector(".wrapper");
    form.addEventListener("input", function() {
      document.querySelector(".save").disabled = false;
    });
    document.querySelector(".save").disabled = true;
  }); 
}

/* Upload a local file to LittleFS, forcing its on-device name (e.g. qrg.txt / config.txt),
   then reboot so the device re-reads it. Used by the qrg.html and config.html forms in
   RX_FSK.ino. 'what' is a human description used in the confirmation prompt.
   Relies on showConfirm()/showProgress()/waitForRebootAndReload() from dialog.js. */
function uploadCfgFile(inputId, dest, what) {
  var inp = document.getElementById(inputId);
  if (!inp || !inp.files || inp.files.length === 0) {
    showAlert("Please choose a file first.");
    return;
  }
  var f = inp.files[0];
  showConfirm("This will replace the entire " + what + " on the device with the contents of \"" +
              f.name + "\".\n\nThe device will reboot to apply the change. Continue?")
    .then(function (ok) {
      if (!ok) return;
      var fd = new FormData();
      fd.append("file", f, dest);   // force the destination filename regardless of the picked file's name
      var dlg = showProgress("Uploading " + dest + "…", "Updating " + what);
      // Capture the current boot nonce so the reboot can be detected afterwards.
      fetch("/bootid", { cache: "no-store" })
        .then(function (r) { return r.ok ? r.text() : ""; })
        .catch(function () { return ""; })
        .then(function (before) {
          return fetch("/file", { method: "POST", body: fd })
            .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); })
            .then(function () {
              // Trigger the reboot; the device restarts immediately, so this won't get a response.
              fetch("/control.html", {
                method: "POST",
                headers: { "Content-Type": "application/x-www-form-urlencoded" },
                body: "reboot=1"
              }).catch(function () {});
              // Swap the dialog to the reboot watcher, which reloads once the device is back.
              waitForRebootAndReload((before || "").trim(), "Updating " + what,
                "The " + what + " was uploaded. The device is rebooting to apply it.");
            });
        })
        .catch(function (e) { dlg.close(); showAlert("Upload failed: " + e.message); });
    });
}

/* Used by qrg.html in RX_FSK.ino */
function prep() {
  var stlist=document.querySelectorAll("input.stype");
  for(txt of stlist){
    var val=txt.getAttribute('value'); var nam=txt.getAttribute('name'); 
    if(val=='2') { val='M'; }
    var sel=document.createElement('select');
    sel.setAttribute('name',nam);
    for(stype of stypes) { 
      var opt=document.createElement('option');
      opt.value=stype[0];
      opt.innerHTML=stype[1];
      if(stype[0]==val) { opt.setAttribute('selected','selected'); }
      sel.appendChild(opt);
    }
    txt.replaceWith(sel);
  }
} 

function qrgTable() {
  var tab=document.getElementById("divTable");

  var table = "<table><tr><th>Ch</th><th>Active</th><th>Frequency</th><th>Decoder</th><th>Launchsite</th></tr>";
  for(i=0; i<qrgs.length; i++) {
     var ck = "";
     if(qrgs[i][0]) ck="checked";
     table += "<tr><td class=\"ch\">" + (i+1) + "</td><td class=\"act\"><input name=\"A" + (i+1) + "\" type=\"checkbox\" " + ck + "/></td>";
     table += "<td><input name=\"F" + (i+1) + "\" type=\"text\" size=7 value=\"" + qrgs[i][1] + "\"></td>";
     table += "<td><input class=\"stype\" name=\"T" + (i+1) + "\" value=\"" + qrgs[i][3] + "\"></td>";
     table += "<td><input name=\"S" + (i+1) + "\" type=\"text\" value=\"" + qrgs[i][2] +"\"></td></tr>";
  }
  table += "</table>";
  tab.innerHTML = table;
  prep();
  footer();
}
