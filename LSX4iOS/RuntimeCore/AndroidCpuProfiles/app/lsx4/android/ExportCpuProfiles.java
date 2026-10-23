package app.lsx4.android;

import android.content.Context;
import android.content.SharedPreferences;
import java.io.File;
import java.nio.file.*;
import java.util.*;
import javax.xml.parsers.DocumentBuilderFactory;
import org.w3c.dom.*;

/** Calls the actual Android resolver AND INI writer, without copying GameDB logic. */
public final class ExportCpuProfiles {
    static final class Preferences implements SharedPreferences, SharedPreferences.Editor {
        final Map<String, Object> values = new LinkedHashMap<>();
        Preferences(Path xml) throws Exception {
            var factory = DocumentBuilderFactory.newInstance();
            factory.setFeature("http://apache.org/xml/features/disallow-doctype-decl", true);
            var children = factory.newDocumentBuilder().parse(xml.toFile()).getDocumentElement().getChildNodes();
            for (int i=0; i<children.getLength(); ++i) {
                if (!(children.item(i) instanceof Element)) continue;
                var e=(Element)children.item(i); var key=e.getAttribute("name");
                if (!key.startsWith("cpu.")) continue;
                switch(e.getTagName()) {
                    case "int": values.put(key,Integer.parseInt(e.getAttribute("value"))); break;
                    case "boolean": values.put(key,Boolean.parseBoolean(e.getAttribute("value"))); break;
                    case "set":
                        Set<String> strings=new LinkedHashSet<>();
                        var items=e.getElementsByTagName("string");
                        for(int n=0;n<items.getLength();++n) strings.add(items.item(n).getTextContent());
                        values.put(key,strings); break;
                    default: throw new IllegalArgumentException("Unsupported CPU preference type: "+key);
                }
            }
        }
        public int getInt(String k,int d) { return (Integer)values.getOrDefault(k,d); }
        public boolean getBoolean(String k,boolean d) { return (Boolean)values.getOrDefault(k,d); }
        @SuppressWarnings("unchecked")
        public Set<String> getStringSet(String k,Set<String> d) { return (Set<String>)values.getOrDefault(k,d); }
        public boolean contains(String k) { return values.containsKey(k); }
        public Map<String,?> getAll() { return values; }
        public Editor edit() { return this; }
        public Editor putInt(String k,int v) { values.put(k,v);return this; }
        public Editor putBoolean(String k,boolean v) { values.put(k,v);return this; }
        public Editor putStringSet(String k,Set<String> v) { values.put(k,new LinkedHashSet<>(v));return this; }
        public Editor remove(String k) { values.remove(k);return this; }
        public boolean commit() { return true; }
    }
    public static void main(String[] args) throws Exception {
        Path prefs=Path.of(args[0]),out=Path.of(args[1]);
        Files.createDirectories(out);
        Context context=new Context(){ public File getFilesDir(){return out.resolve("writer").toFile();} };
        for(int i=2;i<args.length;++i) {
            String title=args[i];
            if(!title.equals("global") && !title.matches("(?:CUSA|PPSA)[0-9]{5}"))
                throw new IllegalArgumentException("Invalid title ID");
            // Rebuild requests may reset an in-memory preference; each export
            // starts from the same snapshot, never changing the Vivo snapshot.
            SettingsActivity.preferences=new Preferences(prefs);
            File generated=CpuTuningSettings.materialize(context,
                title.equals("global") ? "" : "/games/"+title+"/eboot.bin");
            Files.move(generated.toPath(),out.resolve(title+".ini"),StandardCopyOption.REPLACE_EXISTING);
        }
    }
}
