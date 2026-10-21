import gi, sys
gi.require_version('Gtk','3.0')
from gi.repository import Gtk
w=Gtk.Window(title="OC Test App"); b=Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
l=Gtk.Label(label="count=0"); e=Gtk.Entry(); btn=Gtk.Button(label="Press me"); n=[0]
def on(_): n[0]+=1; l.set_text(f"count={n[0]}"); print("pressed", n[0], flush=True)
btn.connect("clicked", on); b.add(l); b.add(e); b.add(btn); w.add(b); w.connect("destroy", Gtk.main_quit); w.show_all(); Gtk.main()
