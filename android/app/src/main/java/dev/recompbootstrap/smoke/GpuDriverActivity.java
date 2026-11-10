package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.graphics.Color;
import android.view.Gravity;
import android.widget.*;
import java.util.List;

/** Native UI for importing and selecting app-private ADPKG/Turnip drivers. */
public final class GpuDriverActivity extends Activity {
    private static final int PICK=7314;
    private TurnipDriverStore store; private LinearLayout list; private TextView status;
    @Override public void onCreate(Bundle b){super.onCreate(b);store=new TurnipDriverStore(this);getWindow().setStatusBarColor(0xFF12171F);getWindow().setNavigationBarColor(0xFF12171F);build();refresh();}
    private int dp(int v){return Math.round(v*getResources().getDisplayMetrics().density);}
    private TextView label(String s,int size,int color){TextView t=new TextView(this);t.setText(s);t.setTextSize(size);t.setTextColor(color);t.setPadding(dp(4),dp(8),dp(4),dp(8));return t;}
    private Button button(String s,Runnable r){Button b=new Button(this);b.setText(s);b.setAllCaps(false);b.setOnClickListener(v->r.run());return b;}
    private void build(){
        LinearLayout page=new LinearLayout(this);page.setOrientation(1);page.setPadding(dp(20),dp(16),dp(20),dp(16));page.setBackgroundColor(0xFF12171F);
        page.addView(label("GPU Driver Settings",24,Color.WHITE));
        page.addView(label("Import an AdrenoTools ADPKG ZIP. The driver is installed in private app storage and loads on the next game start.",14,0xFFCBD5E1));
        page.addView(button("Import Turnip / ADPKG ZIP",this::pick));
        page.addView(button("Use System Vulkan",this::system));
        status=label("Installed drivers",14,0xFF9FB3C8);page.addView(status);
        ScrollView scroll=new ScrollView(this);list=new LinearLayout(this);list.setOrientation(1);scroll.addView(list);page.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
        page.addView(button("Back",this::finish));setContentView(page);
    }
    private void pick(){Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT);i.addCategory(Intent.CATEGORY_OPENABLE);i.setType("*/*");i.putExtra(Intent.EXTRA_TITLE,"Select Turnip ADPKG ZIP");startActivityForResult(i,PICK);}
    @Override protected void onActivityResult(int req,int result,Intent data){super.onActivityResult(req,result,data);if(req!=PICK||result!=RESULT_OK||data==null||data.getData()==null)return;
        status.setText("Installing driver…");android.net.Uri uri=data.getData();
        new Thread(()->{String msg;boolean ok=false;try{TurnipDriverStore.Driver d=store.install(this,uri);store.select(d);msg="Installed and selected: "+d.label();ok=true;}catch(Exception e){msg="Import failed: "+e.getMessage();}
            final String out=msg;final boolean success=ok;runOnUiThread(()->{status.setText(out);refreshList();if(success)new AlertDialog.Builder(this).setTitle("Turnip driver ready").setMessage(out+"\nRestart the game to activate it.").setPositiveButton("OK",null).show();});
        },"turnip-import").start();
    }
    private void system(){try{store.useSystem();status.setText("System Vulkan selected. Restart the game to apply.");refreshList();}catch(Exception e){status.setText("Could not switch: "+e.getMessage());}}
    private void refresh(){refreshList();}
    private void refreshList(){list.removeAllViews();List<TurnipDriverStore.Driver> ds=store.list();if(ds.isEmpty())list.addView(label("No custom drivers installed yet.",14,0xFF9FB3C8));
        for(TurnipDriverStore.Driver d:ds){boolean active=false;try{active=store.selected(d);}catch(Exception ignored){}final TurnipDriverStore.Driver item=d;
            list.addView(label(d.label()+(active?" · ACTIVE":""),16,active?0xFF8FE3B0:Color.WHITE));
            list.addView(button(active?"Selected":"Select this driver",()->{try{store.select(item);status.setText(item.label()+" selected. Restart the game to activate.");refreshList();}catch(Exception e){status.setText("Selection failed: "+e.getMessage());}}));
        }
    }
}
