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
                    if(++count>MAX_ENTRIES)throw new Exception("ZIP has too many files");
                    String n=e.getName();if(e.isDirectory()||!safe(n)||!names.add(n))throw new Exception("Only unique files at ZIP root are accepted");
                    if("meta.json".equals(n)){
                        byte[] b=readEntry(z,64*1024);Object parsed=JsonLite.parse(new String(b,StandardCharsets.UTF_8));
                        if(!(parsed instanceof Map))throw new Exception("meta.json must be an object");m=(Map<?,?>)parsed;
                        try(FileOutputStream metadata=new FileOutputStream(new File(stage,"meta.json"))){metadata.write(b);metadata.getFD().sync();}
                    }else if(n.endsWith(".so")){
                        File dest=new File(stage,n);long size=copyEntry(z,dest,Math.min(MAX_LIBRARY,MAX_TOTAL-total));verifyElf(dest);total+=size;
                        if(total>MAX_TOTAL)throw new Exception("Driver package is too large");
                    }else throw new Exception("Unexpected file in driver ZIP: "+n);
                    z.closeEntry();
                }
            }
            if(m==null||num(m,"schemaVersion")!=1)throw new Exception("Missing or unsupported ADPKG metadata");
            String name=str(m,"name"),author=str(m,"author"),vendor=str(m,"vendor"),version=str(m,"driverVersion");
            str(m,"packageVersion");long minApi=num(m,"minApi");String lib=libraryName(m,stage);
            if(name.isEmpty()||author.isEmpty()||!("Qualcomm".equals(vendor)||"Mesa".equals(vendor))
                    ||!version.matches("[0-9]+\\.[0-9]+\\.[0-9]+(-.+)?")||!safe(lib)||!lib.endsWith(".so"))
                throw new Exception("ADPKG metadata is incomplete or does not match the ADPKG schema");
            if(minApi>Build.VERSION.SDK_INT)throw new Exception("Driver requires Android API "+minApi);
            File main=new File(stage,lib);if(!main.isFile()||main.length()==0||main.length()>MAX_LIBRARY)throw new Exception("Main Vulkan library is missing");
            verifyElf(main);
            String slug=slug(name+"-"+version)+"-"+Long.toHexString(System.nanoTime());
            File dest=new File(root,slug);if(!stage.renameTo(dest))throw new Exception("Could not finish driver installation");
            return new Driver(slug,name,vendor,version,lib);
        }catch(Exception ex){deleteTree(stage);throw ex;}
    }
    private static String libraryName(Map<?,?> metadata,File stage)throws Exception{
        Object declared=metadata.get("libraryName");
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
        if(candidates.size()!=1)throw new Exception("ADPKG must identify one main Vulkan library");
        return candidates.get(0);
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
