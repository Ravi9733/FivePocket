package android.view;
public class Surface {
    public boolean valid=true; public int validFor=Integer.MAX_VALUE;   // validFor: how many more isValid() calls answer true
    public boolean isValid(){return valid && validFor-->0;}
}
