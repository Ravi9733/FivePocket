package dev.recompbootstrap.smoke;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.util.AtomicFile;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.*;

/** Installs root-level AdrenoTools ADPKG ZIPs into app-private storage. */
final class TurnipDriverStore {
    static final String FOLDER="gpu_drivers";
    private static final long MAX_TOTAL=700L*1024*1024, MAX_LIBRARY=512L*1024*1024;
    private static final int MAX_ENTRIES=96;
    static final class Driver {
        final String folder,name,vendor,version,library;
        Driver(String f,String n,String v,String ver,String lib){folder=f;name=n;vendor=v;version=ver;library=lib;}
        String label(){return name+" · "+vendor+" "+version;}
    }
    private final File root;
    TurnipDriverStore(Context c){root=new File(c.getFilesDir(),FOLDER);}
    List<Driver> list(){
        List<Driver> out=new ArrayList<>();File[] dirs=root.listFiles();if(dirs==null)return out;
        for(File d:dirs){if(!d.isDirectory()||d.getName().startsWith(".")||"selected".equals(d.getName()))continue;
            try{Map<?,?> m=meta(new File(d,"meta.json"));String lib=libraryName(m,d);File so=new File(d,lib);
                if(safe(lib)&&so.isFile()&&so.length()>0)out.add(new Driver(d.getName(),str(m,"name"),str(m,"vendor"),str(m,"driverVersion"),lib));
            }catch(Exception ignored){}
        }return out;
    }
    boolean selected(Driver d)throws Exception{
        File f=new File(root,"selected");if(!f.isFile())return false;
        String[] lines=new String(readFile(f,1024),StandardCharsets.UTF_8).split("\\n");
        return lines.length>=2&&d.folder.equals(lines[0].trim())&&d.library.equals(lines[1].trim());
    }
    void useSystem()throws Exception{
        if(!root.exists()&&!root.mkdirs())throw new Exception("Cannot create private driver folder");
        File f=new File(root,"selected");if(f.exists()&&!f.delete())throw new Exception("Cannot clear driver selection");
    }
    void select(Driver d)throws Exception{
        File dir=new File(root,d.folder),so=new File(dir,d.library);
        if(!safe(d.folder)||!safe(d.library)||!dir.isDirectory()||!so.isFile()||so.length()==0||so.length()>MAX_LIBRARY)
            throw new Exception("Driver files are incomplete");
        if(!root.exists()&&!root.mkdirs())throw new Exception("Cannot create private driver folder");
        AtomicFile atomic=new AtomicFile(new File(root,"selected"));
        FileOutputStream out=null;
        try {
            out=atomic.startWrite();
            out.write((d.folder+"\n"+d.library+"\n").getBytes(StandardCharsets.UTF_8));
            atomic.finishWrite(out);
        } catch(Exception failure) {
            if(out!=null)atomic.failWrite(out);
            throw new Exception("Cannot save driver selection",failure);
        }
    }
    Driver install(Context c,Uri uri)throws Exception{
        if(Build.VERSION.SDK_INT<28||!Arrays.asList(Build.SUPPORTED_ABIS).contains("arm64-v8a"))
            throw new Exception("Custom Adreno drivers need Android 9+ and arm64");
        if(!root.exists()&&!root.mkdirs())throw new Exception("Cannot create private driver folder");
        File stage=new File(root,".install-"+Long.toHexString(System.nanoTime()));if(!stage.mkdir())throw new Exception("Cannot create staging folder");
        Map<?,?> m=null;Set<String> names=new HashSet<>();long total=0;int count=0;
        try{
            InputStream raw=c.getContentResolver().openInputStream(uri);if(raw==null)throw new Exception("Cannot open selected ZIP");
            try(ZipInputStream z=new ZipInputStream(new BufferedInputStream(raw))){
                ZipEntry e;while((e=z.getNextEntry())!=null){
                    if(++count>256)throw new Exception("ZIP has too many entries");
                    String path=e.getName();
                    if(e.isDirectory()){z.closeEntry();continue;}
                    String leaf=zipLeaf(path);
                    if(!names.add(leaf))throw new Exception("ZIP contains duplicate file names: "+leaf);
                    if("meta.json".equalsIgnoreCase(leaf)){
                        byte[] b=readEntry(z,64*1024);Object parsed=JsonLite.parse(new String(b,StandardCharsets.UTF_8));
                        if(!(parsed instanceof Map))throw new Exception("meta.json must be a JSON object");m=(Map<?,?>)parsed;
                        try(FileOutputStream metadata=new FileOutputStream(new File(stage,"meta.json"))){metadata.write(b);metadata.getFD().sync();}
                    }else if(leaf.endsWith(".so")){
                        File dest=new File(stage,leaf);long size=copyEntry(z,dest,Math.min(MAX_LIBRARY,MAX_TOTAL-total));verifyElf(dest);total+=size;
                        if(total>MAX_TOTAL)throw new Exception("Driver package is too large");
                    }else if(leaf.length()>255)throw new Exception("ZIP entry name is too long");
                    z.closeEntry();
                }
            }
            String name=optionalString(m,"name"),version=optionalString(m,"driverVersion"),vendor=optionalString(m,"vendor");
            if(m!=null){
                Object schema=m.get("schemaVersion");
                if(schema!=null&&(!(schema instanceof Number)||((Number)schema).intValue()!=1))
                    throw new Exception("Unsupported ADPKG schemaVersion");
            }
            if(name.isEmpty())name=displayName(c,uri);
            if(name.toLowerCase(Locale.ROOT).endsWith(".zip"))name=name.substring(0,name.length()-4);
            if(name.isEmpty())name="Imported Turnip driver";
            if(name.length()>128)name=name.substring(0,128);
            if(version.isEmpty())version="0.0.0";
            if(version.matches("v[0-9].*"))version=version.substring(1);
            if(!version.matches("[0-9]+\\.[0-9]+\\.[0-9]+(-[A-Za-z0-9.+_-]+)?"))version="0.0.0";
            if(vendor.isEmpty())vendor="Mesa";
            if(!"Qualcomm".equals(vendor)&&!"Mesa".equals(vendor))vendor="Mesa";
            long minApi=optionalNumber(m,"minApi",28);
            if(minApi>Build.VERSION.SDK_INT)throw new Exception("Driver requires Android API "+minApi);
            String lib=libraryName(m,stage);
            if(!safe(lib)||!lib.endsWith(".so"))throw new Exception("Main Vulkan library name is invalid");
            File main=new File(stage,lib);if(!main.isFile()||main.length()==0||main.length()>MAX_LIBRARY)throw new Exception("Main Vulkan library is missing");
            verifyElf(main);
            // Always persist normalized metadata. Plain Mesa/Turnip ZIPs often omit meta.json;
            // without this record list() hides the successfully imported driver after restart.
            writeMetadata(new File(stage,"meta.json"),name,vendor,version,lib,minApi);
            String slug=slug(name+"-"+version)+"-"+Long.toHexString(System.nanoTime());
            File dest=new File(root,slug);if(!stage.renameTo(dest))throw new Exception("Could not finish driver installation");
            return new Driver(slug,name,vendor,version,lib);
        }catch(Exception ex){deleteTree(stage);throw ex;}
    }
    private static String zipLeaf(String path)throws Exception{
        if(path==null||path.isEmpty()||path.startsWith("/")||path.indexOf('\\')>=0)
            throw new Exception("Unsafe ZIP entry path");
        String[] parts=path.split("/");
        for(String part:parts)if(part.isEmpty()||".".equals(part)||"..".equals(part))
            throw new Exception("Unsafe ZIP entry path");
        String leaf=parts[parts.length-1];
        if(!safe(leaf))throw new Exception("Unsupported ZIP entry name: "+leaf);
        return leaf;
    }
    private static String optionalString(Map<?,?> m,String key){
        if(m==null)return "";Object v=m.get(key);return v instanceof String?((String)v).trim():"";
    }
    private static long optionalNumber(Map<?,?> m,String key,long fallback)throws Exception{
        if(m==null||!m.containsKey(key))return fallback;
        Object v=m.get(key);if(!(v instanceof Number))throw new Exception("ADPKG field must be numeric: "+key);
        double d=((Number)v).doubleValue();
        if(!Double.isFinite(d)||d!=Math.rint(d)||d<Long.MIN_VALUE||d>Long.MAX_VALUE)
            throw new Exception("ADPKG field must be an integer: "+key);
        return (long)d;
    }
    private static String displayName(Context c,Uri uri){
        try(android.database.Cursor cursor=c.getContentResolver().query(uri,new String[]{android.provider.OpenableColumns.DISPLAY_NAME},null,null,null)){
            if(cursor!=null&&cursor.moveToFirst()){String value=cursor.getString(0);if(value!=null)return value;}
        }catch(Exception ignored){}
        return "Imported Turnip driver";
    }
    private static String libraryName(Map<?,?> metadata,File stage)throws Exception{
        Object declared=metadata==null?null:metadata.get("libraryName");
        if(declared instanceof String){
            String value=((String)declared).trim();
            if(safe(value)&&value.endsWith(".so"))return value;
            throw new Exception("ADPKG libraryName is invalid");
        }
        File[] files=stage.listFiles();
        List<String> candidates=new ArrayList<>();
        if(files!=null)for(File file:files){
            String n=file.getName();
            if(file.isFile()&&n.endsWith(".so")&&(n.startsWith("libvulkan")||n.startsWith("vulkan.")))
                candidates.add(n);
        }
        // Common Turnip distributions use libvulkan_freedreno.so; prefer it when a ZIP
        // contains several Vulkan variants, then use the other known Mesa/Adreno sonames.
        String[] preferred={"libvulkan_freedreno.so","libvulkan_turnip.so","vulkan.adreno.so"};
        for(String candidate:preferred)if(candidates.contains(candidate))return candidate;
        if(candidates.size()!=1)throw new Exception("ADPKG must identify one main Vulkan library");
        return candidates.get(0);
    }
    private static void writeMetadata(File file,String name,String vendor,String version,String library,long minApi)throws Exception{
        String json="{\n"
                +"  \"schemaVersion\": 1,\n"
                +"  \"name\": \""+jsonEscape(name)+"\",\n"
                +"  \"vendor\": \""+jsonEscape(vendor)+"\",\n"
                +"  \"driverVersion\": \""+jsonEscape(version)+"\",\n"
                +"  \"minApi\": "+minApi+",\n"
                +"  \"libraryName\": \""+jsonEscape(library)+"\"\n"
                +"}\n";
        try(FileOutputStream out=new FileOutputStream(file)){out.write(json.getBytes(StandardCharsets.UTF_8));out.getFD().sync();}
    }
    private static String jsonEscape(String value){
        StringBuilder out=new StringBuilder(value.length()+16);
        for(int i=0;i<value.length();i++){
            char c=value.charAt(i);
            switch(c){
                case '\\': out.append("\\\\"); break;
                case '"': out.append("\\\""); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if(c<0x20)out.append(String.format(Locale.ROOT,"\\u%04x",(int)c));
                    else out.append(c);
            }
        }
        return out.toString();
    }
    private static Map<?,?> meta(File f)throws Exception{
        if(!f.isFile()||f.length()>65536)throw new Exception("Metadata missing");
        Object o=JsonLite.parse(new String(readFile(f,65536),StandardCharsets.UTF_8));if(!(o instanceof Map))throw new Exception("Bad metadata");return (Map<?,?>)o;
    }
    private static String str(Map<?,?> m,String k)throws Exception{Object v=m.get(k);if(!(v instanceof String))throw new Exception("ADPKG field missing: "+k);return ((String)v).trim();}
    private static long num(Map<?,?> m,String k)throws Exception{
        Object v=m.get(k);
        if(!(v instanceof Number))throw new Exception("ADPKG field missing: "+k);
        double d=((Number)v).doubleValue();
        if(!Double.isFinite(d)||d!=Math.rint(d)||d<Long.MIN_VALUE||d>Long.MAX_VALUE)
            throw new Exception("ADPKG field must be an integer: "+k);
        return (long)d;
    }
    private static boolean safe(String s){
        if(s==null||s.isEmpty()||s.length()>128||".".equals(s)||"..".equals(s))return false;
        for(int i=0;i<s.length();i++){char c=s.charAt(i);if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='+'||c=='-'))return false;}return true;
    }
    private static String slug(String s){StringBuilder b=new StringBuilder();for(int i=0;i<s.length()&&b.length()<48;i++){char c=Character.toLowerCase(s.charAt(i));b.append((c>='a'&&c<='z')||(c>='0'&&c<='9')?c:'-');}while(b.length()>0&&b.charAt(b.length()-1)=='-')b.setLength(b.length()-1);return b.length()==0?"turnip":b.toString();}
    private static byte[] readEntry(InputStream in,int max)throws Exception{ByteArrayOutputStream b=new ByteArrayOutputStream();byte[] x=new byte[8192];int n;while((n=in.read(x))!=-1){if(b.size()+n>max)throw new Exception("Metadata too large");b.write(x,0,n);}return b.toByteArray();}
    private static byte[] readFile(File f,int max)throws Exception{try(InputStream in=new FileInputStream(f)){return readEntry(in,max);}}
    private static long copyEntry(InputStream in,File f,long max)throws Exception{long total=0;byte[] b=new byte[65536];try(FileOutputStream o=new FileOutputStream(f)){int n;while((n=in.read(b))!=-1){total+=n;if(total>max)throw new Exception("Driver library exceeds size limit");o.write(b,0,n);}o.getFD().sync();}if(total==0)throw new Exception("Empty driver library");return total;}
    private static void verifyElf(File f)throws Exception{
        try(InputStream in=new FileInputStream(f)){
            byte[] h=new byte[20];int at=0,n;
            while(at<h.length&&(n=in.read(h,at,h.length-at))>0)at+=n;
            int type=at==h.length?((h[16]&255)|((h[17]&255)<<8)):0;
            if(at!=h.length||h[0]!=0x7f||h[1]!='E'||h[2]!='L'||h[3]!='F'||h[4]!=2||h[5]!=1
                    ||h[6]!=1||type!=3||(h[18]&255)!=183||(h[19]&255)!=0)
                throw new Exception("Vulkan library is not a 64-bit ARM ELF shared object");
        }
    }
    private static void deleteTree(File f){File[] c=f.listFiles();if(c!=null)for(File x:c)deleteTree(x);f.delete();}
}
