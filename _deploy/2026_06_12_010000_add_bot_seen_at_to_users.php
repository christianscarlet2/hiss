<?php

use Illuminate\Database\Migrations\Migration;
use Illuminate\Database\Schema\Blueprint;
use Illuminate\Support\Facades\Schema;

// A machine (Hiss / API bot) marks this timestamp every time it polls or acts
// through the bot token. The web client treats a recent value as "a bot is
// connected to my seat" and hands the controls over to it.
return new class extends Migration {
    public function up(): void
    {
        Schema::table('users', function (Blueprint $t) {
            if (!Schema::hasColumn('users', 'bot_seen_at')) {
                $t->timestamp('bot_seen_at')->nullable()->after('last_seen_at');
            }
        });
    }

    public function down(): void
    {
        Schema::table('users', function (Blueprint $t) {
            if (Schema::hasColumn('users', 'bot_seen_at')) {
                $t->dropColumn('bot_seen_at');
            }
        });
    }
};
