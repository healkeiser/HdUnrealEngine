# Live Render and Copernicus

The Live Render LOP accepts **HdUnrealEngine** as its render delegate. It renders inside the Houdini process on its own threads, while UE runs on the thread hdUE started for it. The delegate queues its UE work for that thread, which runs it after each UE tick; a render whose work is not picked up within 5 s fails with an error rather than blocking. A File COP with **Source: Live Render** can start and stop it for compositing in Copernicus.

The backend supports one render session per process, so a Live Render and an hdUE Solaris viewport cannot run at the same time (from the code; not tested). The Live Render has been checked to render and converge with its AOVs; the image reaching the File COP has not been verified by the automated tests (in the scripted test the File COP stayed empty with Karma too), so check it in the UI.
